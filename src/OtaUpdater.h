#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaConfig.h"
#include "OtaHash.h"
#include "OtaLink.h"
#include "OtaProtocol.h"
#include "OtaStore.h"
#include "OtaTarget.h"
#include "OtaTypes.h"
#include "OtaVerifier.h"

class OtaUpdater;

/** When a freshly installed image is accepted for good. */
enum class OtaConfirmPolicy : uint8_t {
  OnBoot = 0,   // as soon as begin() runs - no protection beyond booting at all
  OnLinkUp,     // when a link is up again: the image can at least be reached
                // and updated a second time. The default.
  Manual,       // the application calls confirm() once its own checks pass
};

/**
 * Everything the updater is told once, at begin().
 *
 * `board` and `version` are required. The board is what an offer must name to
 * be installed here; the version is what this firmware says it is. Neither is
 * read from the image: on an ESP32 under Arduino the app descriptor's version
 * is the core's, not the sketch's, so the sketch states its own.
 */
struct OtaSettings {
  const char* board = "";
  const char* version = "";

  // Bytes held between the link and the flash. Taken when a job starts and
  // given back when it ends, so an idle node holds nothing. Never smaller than
  // a link's maxChunk(); twice that keeps the link busy while flash writes.
  uint32_t bufferSize = 8192;

  // Most bytes written to the target per update(). One 4 kB flash sector is
  // erased per slice at most, which is what bounds the scan time. A PLC with a
  // tight task interval lowers it.
  uint32_t writeSlice = 4096;

  // How long to wait for the next chunk before asking again. This is the
  // first guess only: the updater measures the link's round trip and adapts
  // (RFC 6298, as TCP does), and doubles it after each timeout up to the
  // maximum. A cellular or NB-IoT link with seconds of latency settles on a
  // long timeout by itself instead of re-asking - and paying - for data that
  // was only slow.
  uint32_t requestTimeoutMs = 4000;
  uint32_t minRequestTimeoutMs = 1000;
  uint32_t maxRequestTimeoutMs = 60000;

  // No progress at all for this long while the link is up: the job ends with
  // Timeout. Time, not a count of retries: a cell link that stalls for a
  // minute is normal and must not end an update. While the link is down the
  // job just waits.
  uint32_t stallTimeoutMs = 600000;

  // Progress is saved to the store this often while downloading, so a node
  // that loses power part way through carries on from there when the same
  // job is offered again, instead of paying for the whole image twice.
  // Needs a store and a target that can resume. 0: never.
  uint32_t resumeEveryBytes = 65536;

  // Progress status while downloading, at most this often.
  uint32_t statusIntervalMs = 1000;

  OtaConfirmPolicy confirm = OtaConfirmPolicy::OnLinkUp;
  // A new image not confirmed within this long is rolled back. 0: no limit
  // (the bootloader still rolls back if the image never boots far enough).
  uint32_t confirmWindowMs = 120000;

  // From the boot slot switching to the reboot, so the "applying" status and
  // anything the application saves on OtaEvent::Applying get out first.
  uint32_t rebootDelayMs = 2000;

  bool autoAccept = true;   // off: an offer waits in Offered for accept()
  bool autoApply = true;    // off: a verified image waits in Ready for apply()
  bool allowDowngrade = false;

  // Refuse every offer as Unsigned until a verifier is set. For a verifier
  // that is attached after begin() - an OpenPLC signing block runs after the
  // node block - so there is no moment when an unsigned image gets in.
  bool requireSignature = false;
};

typedef void (*OtaEventFn)(void* context, OtaEvent event, OtaUpdater& updater);

/**
 * The node side: takes offers, pulls an image over a link, writes it into a
 * target, verifies it, switches to it, and confirms or rolls it back.
 *
 *   OtaEsp32Target target;
 *   OtaUpdater ota(target);
 *   OtaMqttDeviceLink link(mqtt, "modbee/node-7/ota");
 *   ota.addLink(link);
 *   ota.begin(settings);          // in setup()
 *   ota.update();                 // in loop(), every pass
 *
 * NOTHING WAITS. update() does a bounded slice of work and returns. Every event
 * is also polled state (state(), result(), progress()), because a PLC block
 * reads, it does not register callbacks.
 */
class OtaUpdater {
public:
  explicit OtaUpdater(OtaTarget& target);
  ~OtaUpdater();

  OtaUpdater(const OtaUpdater&) = delete;
  OtaUpdater& operator=(const OtaUpdater&) = delete;

  // --- Setup, before begin() ---

  /** Remembers jobs across the install reboot. Optional, recommended. */
  void store(OtaStore* store) { _store = store; }

  /** With a verifier, every offer must carry a valid signature. */
  void verifier(OtaVerifier* verifier) { _verifier = verifier; }

  /** Up to MODBEE_OTA_MAX_LINKS. False when full. */
  bool addLink(OtaLink& link);

  OtaResult begin(const OtaSettings& settings);

  /** Every pass of loop(), or every scan. */
  void update();

  void onEvent(OtaEventFn handler, void* context = nullptr) {
    _onEvent = handler;
    _eventContext = context;
  }

  // --- The application's controls ---

  OtaResult accept();     // the offer waiting in Offered
  OtaResult reject();
  OtaResult apply();      // the image waiting in Ready
  OtaResult abort();      // whatever is running, up to Ready
  OtaResult confirm();    // the image under test
  OtaResult rollback();   // the image under test
  void publishStatus() { _statusDirty = true; }

  // --- Polled state ---

  OtaState state() const { return _state; }
  OtaOutcome outcome() const { return _outcome; }
  OtaResult result() const { return _result; }
  bool busy() const { return _state != OtaState::Idle; }
  uint32_t job() const { return _image.job; }
  uint32_t received() const { return _received; }
  uint32_t size() const { return _image.size; }
  uint8_t progress() const { return ota_percent(_received, _image.size); }
  /** The current or last offer. */
  const OtaImage& image() const { return _image; }
  const char* runningVersion() const { return _settings.version; }
  /** Chunks refused for a bad CRC, and requests repeated, over the last job. */
  uint32_t badChunks() const { return _badChunks; }
  uint32_t retries() const { return _totalRetries; }
  /** Bytes that arrived twice: data paid for and thrown away. */
  uint32_t duplicateBytes() const { return _duplicateBytes; }
  /** The link's measured round trip, ms; -1 before the first sample. */
  int32_t roundTripMs() const { return _srtt; }
  /** The request timeout in use now, ms. */
  uint32_t requestTimeoutNowMs() const { return _rto; }
  OtaStatus status() const;

  // --- What links deliver ---

  void handleOffer(OtaLink& from, const OtaImage& image);
  void handleChunk(OtaLink& from, const OtaChunk& chunk);
  void handleCommand(OtaLink& from, uint32_t job, OtaCommand command);
  /** A raw protocol frame. False if it is not one a node acts on. */
  bool handleFrame(OtaLink& from, const uint8_t* frame, size_t length);

private:
  void start(uint32_t resumeFrom = 0);
  void offerPassed();
  void checking();
  void restartFresh();
  void saveProgress();
  void clearProgress();
  void fail(OtaResult result);
  void succeed();
  void enter(OtaState state);
  void emit(OtaEvent event);
  void releaseBuffer();
  void download(uint32_t now);
  void drain();
  void requestMore(uint32_t now);
  void verify();
  void applying(uint32_t now);
  void testing(uint32_t now);
  void sendStatus(uint32_t now);
  void sampleRoundTrip(uint32_t ms);
  void settleTimeout();
  void finishRecord(OtaOutcome outcome, OtaResult result);
  void settle();
  void noteReason(OtaResult reason);
  OtaResult check(const OtaImage& image) const;
  bool anyLinkReady();

  OtaTarget& _target;
  OtaStore* _store = nullptr;
  OtaVerifier* _verifier = nullptr;
  OtaLink* _links[MODBEE_OTA_MAX_LINKS] = {};
  uint8_t _linkCount = 0;
  OtaLink* _jobLink = nullptr;

  OtaSettings _settings;
  bool _begun = false;
  OtaEventFn _onEvent = nullptr;
  void* _eventContext = nullptr;

  OtaState _state = OtaState::Idle;
  OtaOutcome _outcome = OtaOutcome::None;
  OtaResult _result = OtaResult::Ok;
  OtaImage _image;

  // The ring between link and flash. _head is where the oldest unwritten byte
  // is, _count how many there are.
  uint8_t* _ring = nullptr;
  uint32_t _ringSize = 0;
  uint32_t _head = 0;
  uint32_t _count = 0;

  uint32_t _received = 0;      // next offset expected from the link
  uint32_t _written = 0;       // bytes handed to the target
  uint32_t _requestedEnd = 0;  // everything below this has been asked for
  uint32_t _lastActivity = 0;  // last request sent or chunk taken: the RTO clock
  uint32_t _lastProgress = 0;  // last byte taken or written: the stall clock
  bool _gapSeen = false;       // already asked again for this gap
  uint32_t _totalRetries = 0;
  uint32_t _duplicateBytes = 0;

  // Round trip, RFC 6298, in ms. A request is timed only if it is not a
  // repeat (Karn): a chunk after a repeat cannot say which request it answers.
  int32_t _srtt = -1;
  int32_t _rttvar = 0;
  uint32_t _rto = 0;
  bool _timing = false;
  bool _repeating = false;
  uint32_t _timedAt = 0;
  uint32_t _timedOffset = 0;

  // Bytes allowed in flight. Starts at the ring, drops to one chunk after a
  // timeout and grows a chunk per chunk received - so a link that just
  // choked is not flooded again with a whole window of repeats.
  uint32_t _window = 0;

  // Resume after a reset. The record of an interrupted download is kept from
  // begin() until a job starts; a job carrying on from it rebuilds its hash
  // from what is already in flash before asking for anything.
  bool _haveResume = false;
  OtaRecord _resume;
  // The last job installed or rolled back. Never installed again, so a
  // retained offer for it cannot cause an install, roll back, install loop.
  uint32_t _settledJob = 0;
  uint32_t _offeredResume = 0;  // for an offer waiting in Offered
  uint32_t _resumeFrom = 0;     // this job starts here (0: from scratch)
  uint32_t _rehashed = 0;       // bytes of it hashed back so far
  bool _resumedJob = false;     // gets one retry from scratch on a bad hash
  bool _progressSaved = false;
  uint32_t _savedAt = 0;
  uint32_t _badChunks = 0;

  bool _targetBegun = false;
  bool _hashChecked = false;
  bool _activated = false;
  bool _rebootCalled = false;
  uint32_t _rebootAt = 0;
  uint32_t _testDeadline = 0;

  OtaSha256 _sha;

  bool _statusDirty = true;
  uint32_t _lastStatus = 0;
  uint32_t _lastStatusReceived = 0;
};
