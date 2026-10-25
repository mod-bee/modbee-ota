#include "OtaUpdater.h"

#include <new>
#include <string.h>

#include "OtaManifest.h"
#include "OtaPlatform.h"
#include "OtaNoExcept.h"

namespace {

uint32_t smallest(uint32_t a, uint32_t b) { return a < b ? a : b; }

void copyText(char* out, size_t capacity, const char* in) {
  if (!in) in = "";
  size_t n = strlen(in);
  if (n >= capacity) n = capacity - 1;
  memcpy(out, in, n);
  out[n] = '\0';
}

}  // namespace

OtaUpdater::OtaUpdater(OtaTarget& target) : _target(target) {}

OtaUpdater::~OtaUpdater() { releaseBuffer(); }

bool OtaUpdater::addLink(OtaLink& link) {
  for (uint8_t i = 0; i < _linkCount; i++) {
    if (_links[i] == &link) return true;
  }
  if (_linkCount >= MODBEE_OTA_MAX_LINKS) return false;
  _links[_linkCount++] = &link;
  link.attach(this);
  return true;
}

OtaResult OtaUpdater::begin(const OtaSettings& settings) {
  if (!settings.board || !settings.board[0] || !settings.version || !settings.version[0]) {
    return OtaResult::InvalidArgument;
  }
  if (strlen(settings.board) >= MODBEE_OTA_BOARD_MAX ||
      strlen(settings.version) >= MODBEE_OTA_VERSION_MAX) {
    return OtaResult::InvalidArgument;
  }
  if (settings.bufferSize < 256 || settings.writeSlice == 0) return OtaResult::InvalidArgument;
  if (_begun) return OtaResult::Ok;  // safe to call twice

  _settings = settings;
  _begun = true;
  _target.prepare();
  const uint32_t now = ota_millis();

  OtaRecord record;
  const bool haveRecord = _store && _store->load(record) && record.phase != OtaRecord::Empty;

  if (_target.testing()) {
    // Booted into an image that has not been confirmed. Whatever the record
    // says, this is the job under test.
    if (haveRecord) {
      _image.job = record.job;
      copyText(_image.version, sizeof(_image.version), record.version);
    } else {
      copyText(_image.version, sizeof(_image.version), _settings.version);
    }
    _state = OtaState::Testing;
    _testDeadline = now + _settings.confirmWindowMs;
    emit(OtaEvent::Testing);
    if (_settings.confirm == OtaConfirmPolicy::OnBoot) confirm();
  } else if (haveRecord && record.phase == OtaRecord::Applied) {
    // Applied before the reboot, not under test now. Either the target has no
    // trial boot and the new image simply runs, or we are the old firmware
    // again. Then the record says why, if the new image got to say anything
    // (not_confirmed, rolled_back by request); a crash leaves no reason, and
    // that is a plain rollback by the bootloader.
    _image.job = record.job;
    copyText(_image.version, sizeof(_image.version), record.version);
    if (strcmp(record.version, _settings.version) == 0) {
      finishRecord(OtaOutcome::Success, OtaResult::Ok);
      emit(OtaEvent::Succeeded);
    } else {
      finishRecord(OtaOutcome::Failed,
                   record.result != OtaResult::Ok ? record.result : OtaResult::RolledBack);
      emit(OtaEvent::Failed);
    }
  } else if (haveRecord && record.phase == OtaRecord::Downloading) {
    // A download a reset interrupted. It carries on if the same image is
    // offered again; status says how far it got.
    _haveResume = true;
    _progressSaved = true;  // a job that starts and fails clears it
    _resume = record;
    _image.job = record.job;
    _image.size = record.size;
    copyText(_image.version, sizeof(_image.version), record.version);
    _received = record.written;
  } else if (haveRecord && record.phase == OtaRecord::Finished) {
    _image.job = record.job;
    copyText(_image.version, sizeof(_image.version), record.version);
    _outcome = record.outcome;
    _result = record.result;
    settle();
  }

  _statusDirty = true;
  return OtaResult::Ok;
}

// --- Links in ---

OtaResult OtaUpdater::check(const OtaImage& image) const {
  if (image.job == 0 || image.size == 0) return OtaResult::InvalidArgument;
  if (strcmp(image.board, _settings.board) != 0) return OtaResult::WrongBoard;
  if (image.size > _target.capacity()) return OtaResult::TooLarge;
  const int order = ota_compareVersions(image.version, _settings.version);
  if (order == 0 && !(image.flags & MODBEE_OTA_FLAG_FORCE)) return OtaResult::SameVersion;
  if (order < 0 && !_settings.allowDowngrade) return OtaResult::Downgrade;
  // The signature itself is checked in update(), state Checking: it can take
  // a second, and an offer arrives inside the link's receive callback.
  if (_settings.requireSignature && !_verifier) return OtaResult::Unsigned;
  if (_verifier && image.signatureLength == 0) return OtaResult::Unsigned;
  return OtaResult::Ok;
}

void OtaUpdater::handleOffer(OtaLink& from, const OtaImage& image) {
  if (!_begun) return;
  if (_state != OtaState::Idle) {
    // The same job offered again - a server that restarted, or a retained
    // offer seen on reconnect - carries on where it is. Anything else waits:
    // the status that goes back names the job that is running, which is how
    // a server learns this node is busy.
    if (image.job == _image.job && _state == OtaState::Downloading) {
      _jobLink = &from;
      _requestedEnd = _received;  // ask again from where we are
    }
    _statusDirty = true;
    return;
  }

  // An offer for a job this node has already installed - a retained offer read
  // again after the install reboot - is answered, not repeated. That includes
  // one that was rolled back: installing it again would roll back again, and a
  // retained offer would make that a loop. Any other failure (a timeout, a bad
  // hash) may be retried under the same job number.
  // Kept apart from _image, which a refused offer in between overwrites.
  if (image.job != 0 && image.job == _settledJob) {
    _statusDirty = true;
    return;
  }

  // The download a reset interrupted, offered again: carry on from it.
  const uint32_t resumeFrom =
      (_haveResume && image.job == _resume.job && image.size == _resume.size &&
       memcmp(image.sha256, _resume.sha256, 32) == 0)
          ? _resume.written
          : 0;

  _image = image;
  _jobLink = &from;
  _received = _written = _requestedEnd = 0;
  const OtaResult verdict = check(image);
  if (verdict != OtaResult::Ok) {
    // A refusal is an outcome, reported like any other, so the server hears
    // why without a second channel for it.
    _outcome = OtaOutcome::Failed;
    _result = verdict;
    _statusDirty = true;
    emit(OtaEvent::Failed);
    return;
  }

  _outcome = OtaOutcome::None;
  _result = OtaResult::Ok;
  _offeredResume = resumeFrom;
  if (_verifier) {
    enter(OtaState::Checking);
    return;
  }
  offerPassed();
}

void OtaUpdater::offerPassed() {
  if (_settings.autoAccept) {
    start(_offeredResume);
  } else {
    enter(OtaState::Offered);
    emit(OtaEvent::Offered);
  }
}

void OtaUpdater::checking() {
  uint8_t message[MODBEE_OTA_SIGNED_SIZE];
  ota_signedMessage(_image, message, sizeof(message));
  const OtaResult r =
      _verifier->verify(message, sizeof(message), _image.signature, _image.signatureLength);
  if (r == OtaResult::Pending) return;
  if (r != OtaResult::Ok) {
    _outcome = OtaOutcome::Failed;
    _result = OtaResult::BadSignature;
    enter(OtaState::Idle);
    emit(OtaEvent::Failed);
    return;
  }
  offerPassed();
}

void OtaUpdater::handleChunk(OtaLink& from, const OtaChunk& chunk) {
  (void)from;
  if (_state != OtaState::Downloading || !_ring || chunk.job != _image.job) return;
  if (_rehashed < _resumeFrom) return;  // still rebuilding the hash; the ring is in use
  if (chunk.offset < _received) {
    _duplicateBytes += chunk.length;
    return;
  }
  if (chunk.offset != _received) {
    // A repeat of something already held, or a gap after a lost chunk. A gap
    // is asked for again at once rather than after a timeout - once per gap,
    // since the rest of the window arrives as more of the same gap.
    if (!_gapSeen) {
      _gapSeen = true;
      _repeating = true;
      _timing = false;
      _requestedEnd = _received;
      _totalRetries++;
    }
    return;
  }
  if (ota_crc32(chunk.data, chunk.length) != chunk.crc) {
    _badChunks++;
    if (!_gapSeen) {
      _gapSeen = true;
      _repeating = true;
      _timing = false;
      _requestedEnd = _received;
      _totalRetries++;
    }
    return;
  }
  if (chunk.length == 0 || chunk.length > _image.size - _received ||
      chunk.length > _ringSize - _count) {
    return;  // a server error; the timeout recovers
  }

  // Into the ring, in up to two pieces where it wraps.
  uint32_t tail = (_head + _count) % _ringSize;
  uint32_t first = smallest(chunk.length, _ringSize - tail);
  memcpy(_ring + tail, chunk.data, first);
  if (first < chunk.length) memcpy(_ring, chunk.data + first, chunk.length - first);
  const uint32_t now = ota_millis();
  if (_timing && chunk.offset == _timedOffset) {
    sampleRoundTrip(now - _timedAt);
    _timing = false;
  }
  _count += chunk.length;
  _received += chunk.length;
  if (_requestedEnd < _received) _requestedEnd = _received;
  _lastActivity = _lastProgress = now;
  _gapSeen = false;
  if (_repeating) {
    // Data flows again: drop the backoff, as TCP does on a fresh ACK.
    _repeating = false;
    settleTimeout();
  }
  _window += chunk.length;
  if (_window > _ringSize) _window = _ringSize;
}

void OtaUpdater::sampleRoundTrip(uint32_t ms) {
  // Capped (at about four and a half hours) so the sums below cannot overflow.
  const int32_t r = (int32_t)(ms > 0xffffff ? 0xffffff : ms);
  if (_srtt < 0) {
    _srtt = r;
    _rttvar = r / 2;
  } else {
    const int32_t delta = _srtt > r ? _srtt - r : r - _srtt;
    _rttvar = (3 * _rttvar + delta) / 4;
    _srtt = (7 * _srtt + r) / 8;
  }
  settleTimeout();
}

void OtaUpdater::settleTimeout() {
  uint32_t rto = _settings.requestTimeoutMs;
  if (_srtt >= 0) rto = (uint32_t)_srtt + (uint32_t)(4 * _rttvar > 10 ? 4 * _rttvar : 10);
  if (rto < _settings.minRequestTimeoutMs) rto = _settings.minRequestTimeoutMs;
  if (rto > _settings.maxRequestTimeoutMs) rto = _settings.maxRequestTimeoutMs;
  _rto = rto;
}

void OtaUpdater::handleCommand(OtaLink& from, uint32_t job, OtaCommand command) {
  (void)from;
  if (command == OtaCommand::Status) {
    _statusDirty = true;
    return;
  }
  // 0 means "whatever is running". A command for another job is stale.
  if (job != 0 && job != _image.job) return;
  switch (command) {
    case OtaCommand::Abort: abort(); break;
    case OtaCommand::Apply: apply(); break;
    case OtaCommand::Confirm: confirm(); break;
    case OtaCommand::Rollback: rollback(); break;
    case OtaCommand::Status: break;
  }
  _statusDirty = true;
}

bool OtaUpdater::handleFrame(OtaLink& from, const uint8_t* frame, size_t length) {
  OtaFrameType type;
  if (!ota_frameType(frame, length, type)) return false;
  switch (type) {
    case OtaFrameType::Offer: {
      OtaImage image;
      if (!ota_decodeOffer(frame, length, image)) return false;
      handleOffer(from, image);
      return true;
    }
    case OtaFrameType::Chunk: {
      OtaChunk chunk;
      if (!ota_decodeChunk(frame, length, chunk)) return false;
      handleChunk(from, chunk);
      return true;
    }
    case OtaFrameType::Command: {
      uint32_t job = 0;
      OtaCommand command;
      if (!ota_decodeCommand(frame, length, job, command)) return false;
      handleCommand(from, job, command);
      return true;
    }
    default:
      return false;  // requests and status go the other way
  }
}

// --- The application's controls ---

OtaResult OtaUpdater::accept() {
  if (_state != OtaState::Offered) return OtaResult::NotReady;
  start(_offeredResume);
  return _state == OtaState::Downloading ? OtaResult::Ok : _result;
}

OtaResult OtaUpdater::reject() {
  if (_state != OtaState::Offered) return OtaResult::NotReady;
  fail(OtaResult::Refused);
  return OtaResult::Ok;
}

OtaResult OtaUpdater::apply() {
  if (_state != OtaState::Ready) return OtaResult::NotReady;
  _activated = false;
  _rebootCalled = false;
  enter(OtaState::Applying);
  return OtaResult::Ok;
}

OtaResult OtaUpdater::abort() {
  switch (_state) {
    case OtaState::Checking:
    case OtaState::Offered:
    case OtaState::Downloading:
    case OtaState::Verifying:
    case OtaState::Ready:
      fail(OtaResult::Aborted);
      return OtaResult::Ok;
    case OtaState::Applying:
      // The boot slot is switching or has switched: nothing left to cancel. An
      // activate() that answered Pending may still complete on a worker, so
      // an abort now could leave the new image booting next with no record.
      return OtaResult::Busy;
    default:
      return OtaResult::NotReady;
  }
}

OtaResult OtaUpdater::confirm() {
  if (_state != OtaState::Testing) return OtaResult::NotReady;
  const OtaResult r = _target.confirm();
  if (r != OtaResult::Ok) return r;
  succeed();
  return OtaResult::Ok;
}

OtaResult OtaUpdater::rollback() {
  if (_state != OtaState::Testing) return OtaResult::NotReady;
  // Written first: the old firmware boots next and reads why.
  noteReason(OtaResult::RolledBack);
  const OtaResult r = _target.rollback();
  // Back here only when the target cannot roll back. Still under test, so
  // the reason goes - otherwise the next boot of this very image, or of the
  // old one after a crash, would blame a request that never took effect.
  noteReason(OtaResult::Ok);
  _state = OtaState::Testing;
  _outcome = OtaOutcome::None;
  _result = r;
  return r;
}

OtaStatus OtaUpdater::status() const {
  OtaStatus s;
  s.state = _state;
  s.outcome = _outcome;
  s.result = _result;
  s.job = _image.job;
  s.received = _received;
  s.size = _image.size;
  s.board = _settings.board ? _settings.board : "";
  s.running = _settings.version ? _settings.version : "";
  s.offered = _image.version;
  return s;
}

// --- The work ---

void OtaUpdater::update() {
  for (uint8_t i = 0; i < _linkCount; i++) _links[i]->poll();
  if (!_begun) return;
  const uint32_t now = ota_millis();

  switch (_state) {
    case OtaState::Downloading: download(now); break;
    case OtaState::Verifying: verify(); break;
    case OtaState::Checking: checking(); break;
    case OtaState::Applying: applying(now); break;
    case OtaState::Testing: testing(now); break;
    default: break;
  }
  sendStatus(now);
}

void OtaUpdater::start(uint32_t resumeFrom) {
  // A ring smaller than one chunk would never take one.
  uint32_t size = _settings.bufferSize;
  if (_jobLink && _jobLink->maxChunk() > size) size = _jobLink->maxChunk();
  releaseBuffer();
  _ring = new (std::nothrow) uint8_t[size];
  if (!_ring) {
    fail(OtaResult::NoMemory);
    return;
  }
  _ringSize = size;
  _head = _count = 0;
  _received = _written = _requestedEnd = 0;
  _gapSeen = false;
  _totalRetries = _badChunks = _duplicateBytes = 0;
  _srtt = -1;
  _rttvar = 0;
  _rto = _settings.requestTimeoutMs;
  if (_rto < _settings.minRequestTimeoutMs) _rto = _settings.minRequestTimeoutMs;
  if (_rto > _settings.maxRequestTimeoutMs) _rto = _settings.maxRequestTimeoutMs;
  _timing = _repeating = false;
  _window = size;
  _targetBegun = _hashChecked = _activated = false;
  _sha.reset();
  _haveResume = false;  // whatever happens now replaces the old record
  _resumeFrom = resumeFrom;
  _resumedJob = resumeFrom > 0;
  _rehashed = 0;
  _savedAt = 0;
  _lastActivity = _lastProgress = ota_millis();
  _outcome = OtaOutcome::None;
  _result = OtaResult::Ok;
  enter(OtaState::Downloading);
  emit(OtaEvent::Started);
}

void OtaUpdater::download(uint32_t now) {
  if (!_targetBegun) {
    if (_resumeFrom > 0) {
      uint32_t at = _resumeFrom;
      const OtaResult r = _target.resume(_image, at);
      if (r == OtaResult::Pending) return;
      if (r != OtaResult::Ok || at == 0) {
        // Cannot carry on: from scratch, on the next call.
        _resumeFrom = 0;
        _resumedJob = false;
        return;
      }
      _resumeFrom = at;
    } else {
      const OtaResult r = _target.begin(_image);
      if (r == OtaResult::Pending) return;
      if (r != OtaResult::Ok) {
        fail(r);
        return;
      }
    }
    _targetBegun = true;
    _lastActivity = _lastProgress = now;
  }
  if (_rehashed < _resumeFrom) {
    // Rebuild the hash from what is already in flash, a slice per call, in
    // the ring, which is empty until the first request goes.
    uint32_t n = smallest(smallest(_resumeFrom - _rehashed, _ringSize), _settings.writeSlice);
    if (_target.read(_rehashed, _ring, n) != OtaResult::Ok) {
      restartFresh();
      return;
    }
    _sha.update(_ring, n);
    _rehashed += n;
    _lastProgress = now;
    if (_rehashed == _resumeFrom) {
      _received = _written = _requestedEnd = _savedAt = _resumeFrom;
      _statusDirty = true;
    }
    return;
  }
  drain();
  saveProgress();
  if (_state != OtaState::Downloading) return;  // a write failed
  if (_written == _image.size) {
    releaseBuffer();
    enter(OtaState::Verifying);
    return;
  }
  requestMore(now);
}

void OtaUpdater::drain() {
  uint32_t budget = _settings.writeSlice;
  const uint32_t most = _target.maxWrite();
  while (_count > 0 && budget > 0) {
    // Up to the end of the ring, then round again; never more than the
    // target takes in one call.
    uint32_t n = smallest(smallest(smallest(_count, budget), _ringSize - _head), most);
    const OtaResult r = _target.write(_ring + _head, n);
    if (r == OtaResult::Pending) return;  // the target is busy; same bytes next time
    if (r != OtaResult::Ok) {
      fail(r);
      return;
    }
    _sha.update(_ring + _head, n);
    _lastProgress = ota_millis();
    _head = (_head + n) % _ringSize;
    _count -= n;
    _written += n;
    budget -= n;
  }
}

void OtaUpdater::requestMore(uint32_t now) {
  OtaLink* link = _jobLink;
  if (!link) return;
  if (!link->ready()) {
    // Paused, not failing: both clocks restart when the link is back.
    _lastActivity = _lastProgress = now;
    return;
  }
  if (ota_reached(now, _lastProgress + _settings.stallTimeoutMs)) {
    fail(OtaResult::Timeout);
    return;
  }

  const uint32_t chunk = link->maxChunk() ? link->maxChunk() : 1;
  uint32_t outstanding = _requestedEnd - _received;
  if (outstanding > 0 && ota_reached(now, _lastActivity + _rto)) {
    // Nothing for a whole timeout. Back off, start small, ask again.
    _totalRetries++;
    _rto = _rto * 2 > _settings.maxRequestTimeoutMs ? _settings.maxRequestTimeoutMs : _rto * 2;
    _window = chunk;
    _repeating = true;
    _timing = false;
    _requestedEnd = _received;
    outstanding = 0;
    _lastActivity = now;
  }

  uint32_t window = link->maxWindow();
  if (_window < window) window = _window;
  if (window < chunk) window = chunk;
  const uint32_t unpromised = _ringSize - _count - outstanding;
  const uint32_t remaining = _image.size - _requestedEnd;
  uint32_t want = smallest(unpromised, remaining);
  want = (window > outstanding) ? smallest(want, window - outstanding) : 0;
  if (want == 0) return;

  // Ask in useful amounts: half the ring, or what is left, or what the link
  // allows. A request per drained chunk would double the message count.
  uint32_t worth = _ringSize / 2 > chunk ? _ringSize / 2 : chunk;
  worth = smallest(smallest(worth, remaining), window);
  if (want < worth) return;

  OtaRequest request;
  request.job = _image.job;
  request.offset = _requestedEnd;
  request.length = want;
  request.maxChunk = (uint16_t)smallest(chunk, 0xffffu);
  if (link->sendRequest(request)) {
    if (outstanding == 0) _lastActivity = now;
    // Time one request at a time, never a repeat (Karn).
    if (!_repeating && !_timing) {
      _timing = true;
      _timedAt = now;
      _timedOffset = request.offset;
    }
    _requestedEnd += want;
  }
}

void OtaUpdater::verify() {
  if (!_hashChecked) {
    uint8_t digest[32];
    _sha.finish(digest);
    if (memcmp(digest, _image.sha256, 32) != 0) {
      if (_resumedJob) {
        // What was in flash from before the reset was not what it should
        // have been. Once more, from scratch, before calling it a failure.
        restartFresh();
        return;
      }
      fail(OtaResult::HashMismatch);
      return;
    }
    _hashChecked = true;
  }
  const OtaResult r = _target.finish();
  if (r == OtaResult::Pending) return;
  if (r != OtaResult::Ok) {
    fail(r == OtaResult::IoError ? r : OtaResult::BadImage);
    return;
  }
  enter(OtaState::Ready);
  emit(OtaEvent::Ready);
  if (_state == OtaState::Ready && _settings.autoApply &&
      !(_image.flags & MODBEE_OTA_FLAG_MANUAL_APPLY)) {
    apply();
  }
}

void OtaUpdater::applying(uint32_t now) {
  if (!_activated) {
    const OtaResult r = _target.activate();
    if (r == OtaResult::Pending) return;
    if (r != OtaResult::Ok) {
      fail(r);
      return;
    }
    _activated = true;
    if (_store) {
      OtaRecord record;
      record.phase = OtaRecord::Applied;
      record.job = _image.job;
      copyText(record.version, sizeof(record.version), _image.version);
      copyText(record.previous, sizeof(record.previous), _settings.version);
      _store->save(record);
      _progressSaved = false;  // replaced by the Applied record
    }
    _rebootAt = now + _settings.rebootDelayMs;
    _statusDirty = true;
    emit(OtaEvent::Applying);
    return;
  }
  // Once: on a board it does not return, and a target that defers its
  // restart should not be asked again every pass.
  if (!_rebootCalled && ota_reached(now, _rebootAt)) {
    _rebootCalled = true;
    _target.reboot();
  }
}

void OtaUpdater::testing(uint32_t now) {
  // OnBoot is asked again here too: a target may answer Pending.
  if (_settings.confirm == OtaConfirmPolicy::OnBoot ||
      (_settings.confirm == OtaConfirmPolicy::OnLinkUp && anyLinkReady())) {
    if (confirm() == OtaResult::Ok) return;
  }
  if (_settings.confirmWindowMs > 0 && ota_reached(now, _testDeadline)) {
    noteReason(OtaResult::NotConfirmed);
    _target.rollback();
    // Only back here on a target that cannot roll back. Nothing more to do
    // than keep running; say so once.
    _outcome = OtaOutcome::Failed;
    _result = OtaResult::NotConfirmed;
    settle();
    _state = OtaState::Idle;
    _statusDirty = true;
    emit(OtaEvent::Failed);
  }
}

void OtaUpdater::sendStatus(uint32_t now) {
  bool due = _statusDirty;
  if (!due && _state == OtaState::Downloading && _received != _lastStatusReceived &&
      ota_reached(now, _lastStatus + _settings.statusIntervalMs)) {
    due = true;
  }
  if (!due) return;
  const OtaStatus s = status();
  bool sent = false;
  for (uint8_t i = 0; i < _linkCount; i++) {
    if (_links[i]->ready() && _links[i]->sendStatus(s)) sent = true;
  }
  if (sent) {
    _statusDirty = false;
    _lastStatus = now;
    _lastStatusReceived = _received;
  }
}

// --- Transitions ---

void OtaUpdater::enter(OtaState state) {
  _state = state;
  _statusDirty = true;
}

void OtaUpdater::emit(OtaEvent event) {
  if (_onEvent) _onEvent(_eventContext, event, *this);
}

void OtaUpdater::restartFresh() {
  if (_targetBegun) _target.abort();
  _targetBegun = false;
  clearProgress();
  start(0);
}

void OtaUpdater::saveProgress() {
  if (!_store || _settings.resumeEveryBytes == 0) return;
  // Only what is really in storage: a target with a queue may still hold
  // some of what it has accepted.
  const uint32_t backlog = _target.backlog();
  const uint32_t committed = _written > backlog ? _written - backlog : 0;
  if (committed < _savedAt + _settings.resumeEveryBytes || committed >= _image.size) return;
  OtaRecord record;
  record.phase = OtaRecord::Downloading;
  record.job = _image.job;
  record.size = _image.size;
  record.written = committed;
  memcpy(record.sha256, _image.sha256, 32);
  copyText(record.version, sizeof(record.version), _image.version);
  copyText(record.previous, sizeof(record.previous), _settings.version);
  if (_store->save(record)) {
    _savedAt = committed;
    _progressSaved = true;
  }
}

void OtaUpdater::clearProgress() {
  if (!_progressSaved || !_store) return;
  _store->save(OtaRecord());
  _progressSaved = false;
}

void OtaUpdater::fail(OtaResult result) {
  // While downloading, a begin() or resume() that answered Pending may have
  // started something already; the target is told to drop that too.
  if (_targetBegun || _state == OtaState::Downloading) _target.abort();
  _targetBegun = false;
  clearProgress();
  releaseBuffer();
  _outcome = OtaOutcome::Failed;
  _result = result;
  enter(OtaState::Idle);
  emit(OtaEvent::Failed);
}

void OtaUpdater::succeed() {
  finishRecord(OtaOutcome::Success, OtaResult::Ok);
  enter(OtaState::Idle);
  emit(OtaEvent::Succeeded);
}

void OtaUpdater::finishRecord(OtaOutcome outcome, OtaResult result) {
  _outcome = outcome;
  _result = result;
  settle();
  _statusDirty = true;
  if (!_store) return;
  OtaRecord record;
  _store->load(record);
  record.phase = OtaRecord::Finished;
  record.outcome = outcome;
  record.result = result;
  record.job = _image.job;
  copyText(record.version, sizeof(record.version), _image.version);
  _store->save(record);
}

void OtaUpdater::settle() {
  if (_outcome == OtaOutcome::Success || _result == OtaResult::RolledBack ||
      _result == OtaResult::NotConfirmed) {
    _settledJob = _image.job;
  }
}

void OtaUpdater::noteReason(OtaResult reason) {
  if (!_store) return;
  OtaRecord record;
  if (!_store->load(record) || record.phase != OtaRecord::Applied) return;
  record.result = reason;
  _store->save(record);
}

void OtaUpdater::releaseBuffer() {
  delete[] _ring;
  _ring = nullptr;
  _ringSize = _head = _count = 0;
}

bool OtaUpdater::anyLinkReady() {
  for (uint8_t i = 0; i < _linkCount; i++) {
    if (_links[i]->ready()) return true;
  }
  return false;
}
