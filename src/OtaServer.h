#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaConfig.h"
#include "OtaProtocol.h"
#include "OtaTypes.h"

/**
 * Where a server reads the image it serves.
 *
 * read() may answer Pending: the bytes are not here yet but will be. That is
 * what makes a gateway a relay rather than a store - its source fetches a block
 * from upstream (MQTT, say) on first ask and serves the slices a slow downstream
 * link wants from it. No image ever has to fit on the gateway.
 */
class OtaImageSource {
public:
  virtual ~OtaImageSource() {}
  virtual OtaResult read(uint32_t offset, uint8_t* out, size_t length, size_t& got) = 0;
};

/** An image already in memory or memory-mapped flash. */
class OtaMemorySource : public OtaImageSource {
public:
  OtaMemorySource(const uint8_t* data, uint32_t size) : _data(data), _size(size) {}
  OtaResult read(uint32_t offset, uint8_t* out, size_t length, size_t& got) override;

private:
  const uint8_t* _data;
  uint32_t _size;
};

/** How a server reaches the nodes it serves. A peer is whatever the transport
 *  addresses by: a BeeBus node ID, an index into a table of MQTT topics. */
class OtaServerLink {
public:
  virtual ~OtaServerLink() {}
  virtual bool ready(uint32_t peer) = 0;
  virtual bool sendFrame(uint32_t peer, const uint8_t* frame, size_t length) = 0;
};

/**
 * The other end of OtaUpdater: offers one image to any number of nodes and
 * answers their requests.
 *
 * It keeps almost nothing per node - the range it is part way through sending
 * and the last status heard - because the node drives the transfer. A server
 * that restarts loses nothing a node's next request does not restore.
 *
 * Used by the host tests, by a gateway to feed its downstream nodes, and by
 * any node that updates a neighbour over a cable.
 */
class OtaServer {
public:
  explicit OtaServer(OtaServerLink& link) : _link(link) {}

  /** The image every peer is offered. Resets every peer. */
  bool begin(const OtaImage& image, OtaImageSource& source);

  /** Server-side cap on a chunk, under whatever a node asks for. */
  void chunkSize(uint16_t bytes) { _chunkSize = bytes ? bytes : MODBEE_OTA_CHUNK_MAX; }
  /** Chunks sent per poll(), across all peers - the bound on one call. */
  void chunksPerPoll(uint8_t count) { _chunksPerPoll = count ? count : 1; }

  bool offer(uint32_t peer);
  bool command(uint32_t peer, OtaCommand command);

  /** A frame from a peer: a request or a status. False if it is neither. */
  bool handleFrame(uint32_t peer, const uint8_t* frame, size_t length);

  void poll();

  /** The last status a peer sent, or null if it has said nothing yet. */
  const OtaStatus* peerStatus(uint32_t peer) const;
  const OtaImage& image() const { return _image; }
  uint32_t chunksSent() const { return _chunksSent; }

private:
  struct Peer {
    bool inUse = false;
    bool heard = false;
    uint32_t id = 0;
    uint32_t next = 0;   // next offset to send
    uint32_t end = 0;    // send up to here
    uint16_t maxChunk = 0;
    OtaStatus status;
    char running[MODBEE_OTA_VERSION_MAX] = {};
  };

  Peer* find(uint32_t peer);
  const Peer* find(uint32_t peer) const;
  Peer* claim(uint32_t peer);
  bool sendChunk(Peer& peer);

  OtaServerLink& _link;
  OtaImageSource* _source = nullptr;
  OtaImage _image;
  Peer _peers[MODBEE_OTA_SERVER_PEERS];
  uint8_t _cursor = 0;  // round robin
  uint16_t _chunkSize = MODBEE_OTA_CHUNK_MAX;
  uint8_t _chunksPerPoll = 4;
  uint32_t _chunksSent = 0;
  uint8_t _frame[MODBEE_OTA_CHUNK_HEADER + MODBEE_OTA_CHUNK_MAX];
};
