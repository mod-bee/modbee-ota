#include "OtaServer.h"

#include <string.h>

#include "OtaNoExcept.h"

OtaResult OtaMemorySource::read(uint32_t offset, uint8_t* out, size_t length, size_t& got) {
  got = 0;
  if (!out || offset > _size) return OtaResult::InvalidArgument;
  size_t n = _size - offset;
  if (n > length) n = length;
  if (n) memcpy(out, _data + offset, n);
  got = n;
  return OtaResult::Ok;
}

bool OtaServer::begin(const OtaImage& image, OtaImageSource& source) {
  if (image.job == 0 || image.size == 0) return false;
  _image = image;
  _source = &source;
  for (uint8_t i = 0; i < MODBEE_OTA_SERVER_PEERS; i++) _peers[i] = Peer();
  _chunksSent = 0;
  return true;
}

bool OtaServer::offer(uint32_t peer) {
  if (!_source || !_link.ready(peer)) return false;
  uint8_t frame[MODBEE_OTA_OFFER_MAX];
  const size_t n = ota_encodeOffer(_image, frame, sizeof(frame));
  if (n == 0) return false;
  claim(peer);
  return _link.sendFrame(peer, frame, n);
}

bool OtaServer::command(uint32_t peer, OtaCommand command) {
  if (!_link.ready(peer)) return false;
  uint8_t frame[MODBEE_OTA_COMMAND_SIZE];
  const size_t n = ota_encodeCommand(_image.job, command, frame, sizeof(frame));
  if (command == OtaCommand::Abort) {
    Peer* p = find(peer);
    if (p) p->next = p->end = 0;
  }
  return n && _link.sendFrame(peer, frame, n);
}

bool OtaServer::handleFrame(uint32_t peer, const uint8_t* frame, size_t length) {
  OtaFrameType type;
  if (!ota_frameType(frame, length, type)) return false;

  if (type == OtaFrameType::Request) {
    OtaRequest request;
    if (!ota_decodeRequest(frame, length, request)) return false;
    if (request.job != _image.job || request.offset >= _image.size) return true;
    Peer* p = claim(peer);
    if (!p) return true;
    uint32_t end = request.offset + request.length;
    if (end > _image.size || end < request.offset) end = _image.size;
    // A request that continues the range being sent extends it - a node keeps
    // more than one in flight. Anything else is a node asking again from
    // where it really is, and replaces the range.
    if (p->end > p->next && request.offset == p->end) {
      p->end = end;
    } else {
      p->next = request.offset;
      p->end = end;
    }
    p->maxChunk = request.maxChunk;
    return true;
  }

  if (type == OtaFrameType::Status) {
    Peer* p = claim(peer);
    if (!p) return true;
    OtaStatus status;
    if (!ota_decodeStatus(frame, length, status, p->running, sizeof(p->running))) return false;
    p->status = status;
    p->heard = true;
    return true;
  }
  return false;
}

void OtaServer::poll() {
  if (!_source) return;
  uint8_t sent = 0;
  // Round robin, so one fast node does not starve the rest.
  for (uint8_t step = 0; step < MODBEE_OTA_SERVER_PEERS && sent < _chunksPerPoll; step++) {
    Peer& p = _peers[_cursor];
    _cursor = (uint8_t)((_cursor + 1) % MODBEE_OTA_SERVER_PEERS);
    if (!p.inUse || p.next >= p.end) continue;
    while (p.next < p.end && sent < _chunksPerPoll) {
      if (!sendChunk(p)) break;
      sent++;
    }
  }
}

bool OtaServer::sendChunk(Peer& p) {
  if (!_link.ready(p.id)) return false;
  uint32_t n = p.end - p.next;
  uint32_t cap = _chunkSize;
  if (p.maxChunk && p.maxChunk < cap) cap = p.maxChunk;
  if (cap > MODBEE_OTA_CHUNK_MAX) cap = MODBEE_OTA_CHUNK_MAX;
  if (n > cap) n = cap;

  size_t got = 0;
  uint8_t* payload = _frame + MODBEE_OTA_CHUNK_HEADER;
  const OtaResult r = _source->read(p.next, payload, n, got);
  if (r == OtaResult::Pending || got == 0) return false;  // try again next poll
  if (r != OtaResult::Ok) {
    p.next = p.end;  // nothing to serve; the node's timeout ends it
    return false;
  }
  const size_t length = ota_encodeChunk(_image.job, p.next, payload, got, _frame, sizeof(_frame));
  if (!length || !_link.sendFrame(p.id, _frame, length)) return false;
  p.next += (uint32_t)got;
  _chunksSent++;
  return true;
}

const OtaStatus* OtaServer::peerStatus(uint32_t peer) const {
  const Peer* p = find(peer);
  return (p && p->heard) ? &p->status : nullptr;
}

OtaServer::Peer* OtaServer::find(uint32_t peer) {
  for (uint8_t i = 0; i < MODBEE_OTA_SERVER_PEERS; i++) {
    if (_peers[i].inUse && _peers[i].id == peer) return &_peers[i];
  }
  return nullptr;
}

const OtaServer::Peer* OtaServer::find(uint32_t peer) const {
  for (uint8_t i = 0; i < MODBEE_OTA_SERVER_PEERS; i++) {
    if (_peers[i].inUse && _peers[i].id == peer) return &_peers[i];
  }
  return nullptr;
}

OtaServer::Peer* OtaServer::claim(uint32_t peer) {
  Peer* p = find(peer);
  if (p) return p;
  for (uint8_t i = 0; i < MODBEE_OTA_SERVER_PEERS; i++) {
    if (!_peers[i].inUse) {
      _peers[i] = Peer();
      _peers[i].inUse = true;
      _peers[i].id = peer;
      return &_peers[i];
    }
  }
  return nullptr;
}
