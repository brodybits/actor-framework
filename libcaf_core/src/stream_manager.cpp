/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2018 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/stream_manager.hpp"

#include "caf/actor_addr.hpp"
#include "caf/actor_cast.hpp"
#include "caf/actor_control_block.hpp"
#include "caf/error.hpp"
#include "caf/expected.hpp"
#include "caf/inbound_path.hpp"
#include "caf/scheduled_actor.hpp"
#include "caf/logger.hpp"
#include "caf/message.hpp"
#include "caf/outbound_path.hpp"
#include "caf/response_promise.hpp"
#include "caf/sec.hpp"
#include "caf/stream_scatterer.hpp"

namespace caf {

stream_manager::stream_manager(scheduled_actor* selfptr, stream_priority prio)
    : self_(selfptr),
      pending_handshakes_(0),
      priority_(prio),
      continuous_(false) {
  // nop
}

stream_manager::~stream_manager() {
  // nop
}

void stream_manager::handle(inbound_path*, downstream_msg::batch&) {
  CAF_LOG_WARNING("unimplemented base handler for batches called");
}

void stream_manager::handle(inbound_path*, downstream_msg::close&) {
  // nop
}

void stream_manager::handle(inbound_path*, downstream_msg::forced_close& x) {
  abort(std::move(x.reason));
}

bool stream_manager::handle(stream_slots slots, upstream_msg::ack_open& x) {
  CAF_LOG_TRACE(CAF_ARG(slots) << CAF_ARG(x));
  auto path = out().add_path(slots.invert(), x.rebind_to);
  if (path == nullptr)
    return false;
  path->open_credit = x.initial_demand;
  path->desired_batch_size = x.desired_batch_size;
  --pending_handshakes_;
  push();
  in_flight_promises_.erase(slots.sender);
  return true;
}

void stream_manager::handle(stream_slots slots, upstream_msg::ack_batch& x) {
  CAF_LOG_TRACE(CAF_ARG(slots) << CAF_ARG(x));
  auto path = out().path(slots.receiver);
  if (path != nullptr) {
    path->open_credit += x.new_capacity;
    path->desired_batch_size = x.desired_batch_size;
    path->next_ack_id = x.acknowledged_id + 1;
    push();
  }
}

void stream_manager::handle(stream_slots slots, upstream_msg::drop&) {
  error tmp;
  out().remove_path(slots.receiver, std::move(tmp), true);
}

void stream_manager::handle(stream_slots slots, upstream_msg::forced_drop& x) {
  if (out().remove_path(slots.receiver, x.reason, true))
    abort(std::move(x.reason));
}

void stream_manager::stop() {
  CAF_LOG_TRACE("");
  out().close();
  error tmp;
  finalize(tmp);
  self_->erase_inbound_paths_later(this);
  if (!promises_.empty())
    deliver_promises(make_final_result());
}

void stream_manager::abort(error reason) {
  CAF_LOG_TRACE(CAF_ARG(reason));
  if (!promises_.empty() || !in_flight_promises_.empty()) {
    auto msg = make_message(reason);
    deliver_promises(msg);
    for (auto& kvp : in_flight_promises_)
      kvp.second.deliver(msg);
    in_flight_promises_.clear();
  }
  out().abort(reason);
  finalize(reason);
  self_->erase_inbound_paths_later(this, std::move(reason));
}

void stream_manager::push() {
  CAF_LOG_TRACE("");
  do {
    out().emit_batches();
  } while (generate_messages());
}

bool stream_manager::congested() const noexcept {
  return false;
}

void stream_manager::send_handshake(strong_actor_ptr dest, stream_slot slot,
                                    strong_actor_ptr client,
                                    mailbox_element::forwarding_stack fwd_stack,
                                    message_id mid) {
  CAF_ASSERT(dest != nullptr);
  ++pending_handshakes_;
  in_flight_promises_.emplace(
    slot, response_promise{self()->ctrl(), client, fwd_stack, mid});
  dest->enqueue(
    make_mailbox_element(std::move(client), mid, std::move(fwd_stack),
                         open_stream_msg{slot, make_handshake(slot),
                                         self_->ctrl(), dest, priority_}),
    self_->context());
}

void stream_manager::send_handshake(strong_actor_ptr dest, stream_slot slot) {
  mailbox_element::forwarding_stack fwd_stack;
  send_handshake(std::move(dest), slot, nullptr, std::move(fwd_stack),
                 make_message_id());
}

bool stream_manager::generate_messages() {
  return false;
}

void stream_manager::cycle_timeout(size_t) {
  // TODO: make pure virtual
}

void stream_manager::register_input_path(inbound_path* ptr) {
  CAF_ASSERT(ptr != nullptr);
  CAF_LOG_TRACE(CAF_ARG2("path", *ptr));
  inbound_paths_.emplace_back(ptr);
}

void stream_manager::deregister_input_path(inbound_path* ptr) noexcept {
  CAF_ASSERT(ptr != nullptr);
  CAF_LOG_TRACE(CAF_ARG2("path", *ptr));
  CAF_ASSERT(inbound_paths_.size() > 0);
  using std::swap;
  if (ptr != inbound_paths_.back()) {
    auto i = std::find(inbound_paths_.begin(), inbound_paths_.end(), ptr);
    CAF_ASSERT(i != inbound_paths_.end());
    swap(*i, inbound_paths_.back());
  }
  inbound_paths_.pop_back();
  CAF_LOG_DEBUG(inbound_paths_.size() << "paths remaining");
}

void stream_manager::remove_input_path(stream_slot slot, error reason,
                                       bool silent) {
  if (silent)
    self_->erase_inbound_path_later(slot);
  else
    self_->erase_inbound_path_later(slot, std::move(reason));
}

void stream_manager::add_promise(response_promise x) {
  CAF_LOG_TRACE(CAF_ARG(x));
  CAF_ASSERT(out().terminal());
  promises_.emplace_back(std::move(x));
}

void stream_manager::deliver_promises(message x) {
  CAF_LOG_TRACE(CAF_ARG(x));
  for (auto& p : promises_)
    p.deliver(x);
  promises_.clear();
}

void stream_manager::add_unsafe_outbound_path(
  strong_actor_ptr next, stream_slot slot, strong_actor_ptr origin,
  mailbox_element::forwarding_stack stages, message_id mid) {
  CAF_ASSERT(next != nullptr);
  CAF_ASSERT(out().terminal() == false);
  // Build pipeline by forwarding handshake along the path.
  send_handshake(std::move(next), slot, std::move(origin),
                 std::move(stages), mid);
  generate_messages();
}

stream_slot stream_manager::assign_next_slot() {
  return self_->assign_next_slot_to(this);
}

stream_slot stream_manager::assign_next_pending_slot() {
  return self_->assign_next_pending_slot_to(this);
}

void stream_manager::finalize(const error&) {
  // nop
}

message stream_manager::make_final_result() {
  return none;
}

error stream_manager::process_batch(message&) {
  CAF_LOG_ERROR("stream_manager::process_batch called");
  return sec::invalid_stream_state;
}

void stream_manager::output_closed(error) {
  // nop
}

message stream_manager::make_handshake(stream_slot) const {
  CAF_LOG_ERROR("stream_manager::make_handshake called");
  return none;
}

void stream_manager::downstream_demand(outbound_path*, long) {
  CAF_LOG_ERROR("stream_manager::downstream_demand called");
}

void stream_manager::input_closed(error) {
  // nop
}

stream_slot
stream_manager::add_unsafe_outbound_path_impl(strong_actor_ptr next) {
  CAF_ASSERT(next != nullptr);
  CAF_ASSERT(out().terminal() == false);
  auto slot = assign_next_pending_slot();
  add_unsafe_outbound_path(std::move(next), slot, nullptr,
                           no_stages, make_message_id());
  return slot;
}

} // namespace caf
