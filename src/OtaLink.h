#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaProtocol.h"
#include "OtaTypes.h"

class OtaUpdater;

/**
 * How a node talks to whoever is updating it: MQTT, Sparkplug, BeeBus, a cable.
 *
 * Outbound, the updater calls sendRequest() and sendStatus(). Inbound, the link
 * hands what arrives to the updater it is attached to - an offer, a chunk, a
 * command - through OtaUpdater::handle...(), passing itself so a job stays on
 * the link it came from. A link that carries raw protocol frames hands them to
 * OtaUpdater::handleFrame() and lets the updater decode them.
 *
 * A link is a thin adapter. It owns no buffers it does not need to, and it
 * never waits on its transport: send...() queues or refuses, and the updater
 * retries a refused request on its own schedule.
 */
class OtaLink {
public:
  virtual ~OtaLink() {}

  /** Called by OtaUpdater::addLink(). A link uses it to deliver what arrives. */
  void attach(OtaUpdater* updater) { _updater = updater; }
  OtaUpdater* updater() const { return _updater; }

  /** True when the transport is up and a send has a chance of going out. */
  virtual bool ready() = 0;

  /** The biggest chunk payload this link can deliver in one message. */
  virtual uint16_t maxChunk() const = 0;

  /**
   * The most bytes worth having in flight at once. Unlimited by default; a link
   * that only keeps the latest message (Sparkplug metrics, BeeBus topics)
   * returns maxChunk() so the server sends one chunk per request.
   */
  virtual uint32_t maxWindow() const { return 0xffffffffu; }

  virtual bool sendRequest(const OtaRequest& request) = 0;
  virtual bool sendStatus(const OtaStatus& status) = 0;

  /** Called from every OtaUpdater::update(). Bounded work only. */
  virtual void poll() {}

protected:
  OtaUpdater* _updater = nullptr;
};
