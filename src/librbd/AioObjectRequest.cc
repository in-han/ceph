// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/Mutex.h"
#include "common/RWLock.h"
#include "common/WorkQueue.h"
#include "include/Context.h"

#include "librbd/AioObjectRequest.h"
#include "librbd/AioCompletion.h"
#include "librbd/AioImageRequest.h"
#include "librbd/CopyupRequest.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/ObjectMap.h"
#include "librbd/Utils.h"

#include <boost/bind.hpp>
#include <boost/optional.hpp>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::AioObjectRequest: "

namespace librbd {

template <typename I>
AioObjectRequest<I>*
AioObjectRequest<I>::create_remove(I *ictx, const std::string &oid,
                                   uint64_t object_no,
                                   const ::SnapContext &snapc,
                                   Context *completion) {
  return new AioObjectRemove(util::get_image_ctx(ictx), oid, object_no, snapc,
                             completion);
}

template <typename I>
AioObjectRequest<I>*
AioObjectRequest<I>::create_truncate(I *ictx, const std::string &oid,
                                     uint64_t object_no, uint64_t object_off,
                                     const ::SnapContext &snapc,
                                     Context *completion) {
  return new AioObjectTruncate(util::get_image_ctx(ictx), oid, object_no,
                               object_off, snapc, completion);
}

template <typename I>
AioObjectRequest<I>*
AioObjectRequest<I>::create_write(I *ictx, const std::string &oid,
                                  uint64_t object_no, uint64_t object_off,
                                  const ceph::bufferlist &data,
                                  const ::SnapContext &snapc,
                                  Context *completion, int op_flags) {
  return new AioObjectWrite(util::get_image_ctx(ictx), oid, object_no,
                            object_off, data, snapc, completion, op_flags);
}

template <typename I>
AioObjectRequest<I>*
AioObjectRequest<I>::create_zero(I *ictx, const std::string &oid,
                                 uint64_t object_no, uint64_t object_off,
                                 uint64_t object_len,
                                 const ::SnapContext &snapc,
                                 Context *completion) {
  return new AioObjectZero(util::get_image_ctx(ictx), oid, object_no,
                           object_off, object_len, snapc, completion);
}

template <typename I>
AioObjectRequest<I>::AioObjectRequest(ImageCtx *ictx, const std::string &oid,
                                      uint64_t objectno, uint64_t off,
                                      uint64_t len, librados::snap_t snap_id,
                                      Context *completion, bool hide_enoent)
  : m_ictx(ictx), m_oid(oid), m_object_no(objectno), m_object_off(off),
    m_object_len(len), m_snap_id(snap_id), m_completion(completion),
    m_hide_enoent(hide_enoent) {

  Striper::extent_to_file(m_ictx->cct, &m_ictx->layout, m_object_no,
                          0, m_ictx->layout.object_size, m_parent_extents);

  RWLock::RLocker snap_locker(m_ictx->snap_lock);
  RWLock::RLocker parent_locker(m_ictx->parent_lock);
  compute_parent_extents();
}

template <typename I>
void AioObjectRequest<I>::complete(int r)
{
  if (should_complete(r)) {
    ldout(m_ictx->cct, 20) << "complete " << this << dendl;
    if (m_hide_enoent && r == -ENOENT) {
      r = 0;
    }
    m_completion->complete(r);
    delete this;
  }
}

template <typename I>
bool AioObjectRequest<I>::compute_parent_extents() {
  assert(m_ictx->snap_lock.is_locked());
  assert(m_ictx->parent_lock.is_locked());

  uint64_t parent_overlap;
  int r = m_ictx->get_parent_overlap(m_snap_id, &parent_overlap);
  if (r < 0) {
    // NOTE: it's possible for a snapshot to be deleted while we are
    // still reading from it
    lderr(m_ictx->cct) << this << " compute_parent_extents: failed to "
                       << "retrieve parent overlap: " << cpp_strerror(r)
                       << dendl;
    m_parent_extents.clear();
    return false;
  }

  uint64_t object_overlap =
    m_ictx->prune_parent_extents(m_parent_extents, parent_overlap);
  if (object_overlap > 0) {
    ldout(m_ictx->cct, 20) << this << " compute_parent_extents: "
                           << "overlap " << parent_overlap << " "
                           << "extents " << m_parent_extents << dendl;
    return true;
  }
  return false;
}

static inline bool is_copy_on_read(ImageCtx *ictx, librados::snap_t snap_id) {
  assert(ictx->snap_lock.is_locked());
  return (ictx->clone_copy_on_read &&
          !ictx->read_only && snap_id == CEPH_NOSNAP &&
          (ictx->exclusive_lock == nullptr ||
           ictx->exclusive_lock->is_lock_owner()));
}

/** read **/

template <typename I>
AioObjectRead<I>::AioObjectRead(I *ictx, const std::string &oid,
                                uint64_t objectno, uint64_t offset,
                                uint64_t len,
                                vector<pair<uint64_t,uint64_t> >& be,
                                librados::snap_t snap_id, bool sparse,
                                Context *completion, int op_flags)
  : AioObjectRequest<I>(util::get_image_ctx(ictx), oid, objectno, offset, len,
                        snap_id, completion, false),
    m_buffer_extents(be), m_tried_parent(false), m_sparse(sparse),
    m_op_flags(op_flags), m_state(LIBRBD_AIO_READ_FLAT) {
  guard_read();
}

template <typename I>
void AioObjectRead<I>::guard_read()
{
  ImageCtx *image_ctx = this->m_ictx;
  RWLock::RLocker snap_locker(image_ctx->snap_lock);
  RWLock::RLocker parent_locker(image_ctx->parent_lock);

  if (this->has_parent()) {
    ldout(image_ctx->cct, 20) << __func__ << " guarding read" << dendl;
    m_state = LIBRBD_AIO_READ_GUARD;
  }
}

template <typename I>
bool AioObjectRead<I>::should_complete(int r)
{
  ImageCtx *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "should_complete " << this << " "
                            << this->m_oid << " "
                            << this->m_object_off << "~" << this->m_object_len
                            << " r = " << r << dendl;

  bool finished = true;

  switch (m_state) {
  case LIBRBD_AIO_READ_GUARD:
    ldout(image_ctx->cct, 20) << "should_complete " << this
                              << " READ_CHECK_GUARD" << dendl;

    // This is the step to read from parent
    if (!m_tried_parent && r == -ENOENT) {
      {
        RWLock::RLocker snap_locker(image_ctx->snap_lock);
        RWLock::RLocker parent_locker(image_ctx->parent_lock);
        if (image_ctx->parent == NULL) {
          ldout(image_ctx->cct, 20) << "parent is gone; do nothing" << dendl;
          m_state = LIBRBD_AIO_READ_FLAT;
          finished = false;
          break;
        }

        // calculate reverse mapping onto the image
        vector<pair<uint64_t,uint64_t> > parent_extents;
        Striper::extent_to_file(image_ctx->cct, &image_ctx->layout,
                                this->m_object_no, this->m_object_off,
                                this->m_object_len, parent_extents);

        uint64_t parent_overlap = 0;
        uint64_t object_overlap = 0;
        r = image_ctx->get_parent_overlap(this->m_snap_id, &parent_overlap);
        if (r == 0) {
          object_overlap = image_ctx->prune_parent_extents(parent_extents,
                                                           parent_overlap);
        }

        if (object_overlap > 0) {
          m_tried_parent = true;
          if (is_copy_on_read(image_ctx, this->m_snap_id)) {
            m_state = LIBRBD_AIO_READ_COPYUP;
          }

          read_from_parent(std::move(parent_extents));
          finished = false;
        }
      }
    }
    break;
  case LIBRBD_AIO_READ_COPYUP:
    ldout(image_ctx->cct, 20) << "should_complete " << this << " READ_COPYUP"
                              << dendl;
    // This is the extra step for copy-on-read: kick off an asynchronous copyup.
    // It is different from copy-on-write as asynchronous copyup will finish
    // by itself so state won't go back to LIBRBD_AIO_READ_GUARD.

    assert(m_tried_parent);
    if (r > 0) {
      // If read entire object from parent success and CoR is possible, kick
      // off a asynchronous copyup. This approach minimizes the latency
      // impact.
      send_copyup();
    }
    break;
  case LIBRBD_AIO_READ_FLAT:
    ldout(image_ctx->cct, 20) << "should_complete " << this << " READ_FLAT"
                              << dendl;
    // The read content should be deposit in m_read_data
    break;
  default:
    lderr(image_ctx->cct) << "invalid request state: " << m_state << dendl;
    assert(0);
  }

  return finished;
}

template <typename I>
void AioObjectRead<I>::send() {
  ImageCtx *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "send " << this << " " << this->m_oid << " "
                            << this->m_object_off << "~" << this->m_object_len
                            << dendl;

  {
    RWLock::RLocker snap_locker(image_ctx->snap_lock);

    // send read request to parent if the object doesn't exist locally
    if (image_ctx->object_map != nullptr &&
        !image_ctx->object_map->object_may_exist(this->m_object_no)) {
      image_ctx->op_work_queue->queue(util::create_context_callback<
        AioObjectRequest<I> >(this), -ENOENT);
      return;
    }
  }

  librados::ObjectReadOperation op;
  int flags = image_ctx->get_read_flags(this->m_snap_id);
  if (m_sparse) {
    op.sparse_read(this->m_object_off, this->m_object_len, &m_ext_map,
                   &m_read_data, nullptr);
  } else {
    op.read(this->m_object_off, this->m_object_len, &m_read_data, nullptr);
  }
  op.set_op_flags2(m_op_flags);

  librados::AioCompletion *rados_completion =
    util::create_rados_ack_callback(this);
  int r = image_ctx->data_ctx.aio_operate(this->m_oid, rados_completion, &op,
                                          flags, nullptr);
  assert(r == 0);

  rados_completion->release();
}

template <typename I>
void AioObjectRead<I>::send_copyup()
{
  ImageCtx *image_ctx = this->m_ictx;
  {
    RWLock::RLocker snap_locker(image_ctx->snap_lock);
    RWLock::RLocker parent_locker(image_ctx->parent_lock);
    if (!this->compute_parent_extents() ||
        (image_ctx->exclusive_lock != nullptr &&
         !image_ctx->exclusive_lock->is_lock_owner())) {
      return;
    }
  }

  Mutex::Locker copyup_locker(image_ctx->copyup_list_lock);
  map<uint64_t, CopyupRequest*>::iterator it =
    image_ctx->copyup_list.find(this->m_object_no);
  if (it == image_ctx->copyup_list.end()) {
    // create and kick off a CopyupRequest
    CopyupRequest *new_req = new CopyupRequest(
      image_ctx, this->m_oid, this->m_object_no,
      std::move(this->m_parent_extents));
    this->m_parent_extents.clear();

    image_ctx->copyup_list[this->m_object_no] = new_req;
    new_req->send();
  }
}

template <typename I>
void AioObjectRead<I>::read_from_parent(Extents&& parent_extents)
{
  ImageCtx *image_ctx = this->m_ictx;
  AioCompletion *parent_completion = AioCompletion::create_and_start<
    AioObjectRequest<I> >(this, image_ctx, AIO_TYPE_READ);

  ldout(image_ctx->cct, 20) << "read_from_parent this = " << this
                            << " parent completion " << parent_completion
                            << " extents " << parent_extents
                            << dendl;
  AioImageRequest<>::aio_read(image_ctx->parent, parent_completion,
                              std::move(parent_extents), nullptr, &m_read_data,
                              0);
}

/** write **/

AbstractAioObjectWrite::AbstractAioObjectWrite(ImageCtx *ictx,
                                               const std::string &oid,
                                               uint64_t object_no,
                                               uint64_t object_off,
                                               uint64_t len,
                                               const ::SnapContext &snapc,
                                               Context *completion,
                                               bool hide_enoent)
  : AioObjectRequest(ictx, oid, object_no, object_off, len, CEPH_NOSNAP,
                     completion, hide_enoent),
    m_state(LIBRBD_AIO_WRITE_FLAT), m_snap_seq(snapc.seq.val)
{
  m_snaps.insert(m_snaps.end(), snapc.snaps.begin(), snapc.snaps.end());
}

void AbstractAioObjectWrite::guard_write()
{
  if (has_parent()) {
    m_state = LIBRBD_AIO_WRITE_GUARD;
    m_write.assert_exists();
    ldout(m_ictx->cct, 20) << __func__ << " guarding write" << dendl;
  }
}

bool AbstractAioObjectWrite::should_complete(int r)
{
  ldout(m_ictx->cct, 20) << get_write_type() << " " << this << " " << m_oid
                         << " " << m_object_off << "~" << m_object_len
                         << " should_complete: r = " << r << dendl;

  bool finished = true;
  switch (m_state) {
  case LIBRBD_AIO_WRITE_PRE:
    ldout(m_ictx->cct, 20) << "WRITE_PRE" << dendl;
    if (r < 0) {
      return true;
    }

    send_write();
    finished = false;
    break;

  case LIBRBD_AIO_WRITE_POST:
    ldout(m_ictx->cct, 20) << "WRITE_POST" << dendl;
    finished = true;
    break;

  case LIBRBD_AIO_WRITE_GUARD:
    ldout(m_ictx->cct, 20) << "WRITE_CHECK_GUARD" << dendl;

    if (r == -ENOENT) {
      handle_write_guard();
      finished = false;
      break;
    } else if (r < 0) {
      // pass the error code to the finish context
      m_state = LIBRBD_AIO_WRITE_ERROR;
      complete(r);
      finished = false;
      break;
    }

    finished = send_post();
    break;

  case LIBRBD_AIO_WRITE_COPYUP:
    ldout(m_ictx->cct, 20) << "WRITE_COPYUP" << dendl;
    if (r < 0) {
      m_state = LIBRBD_AIO_WRITE_ERROR;
      complete(r);
      finished = false;
    } else {
      finished = send_post();
    }
    break;

  case LIBRBD_AIO_WRITE_FLAT:
    ldout(m_ictx->cct, 20) << "WRITE_FLAT" << dendl;

    finished = send_post();
    break;

  case LIBRBD_AIO_WRITE_ERROR:
    assert(r < 0);
    lderr(m_ictx->cct) << "WRITE_ERROR: " << cpp_strerror(r) << dendl;
    break;

  default:
    lderr(m_ictx->cct) << "invalid request state: " << m_state << dendl;
    assert(0);
  }

  return finished;
}

void AbstractAioObjectWrite::send() {
  ldout(m_ictx->cct, 20) << "send " << get_write_type() << " " << this <<" "
                         << m_oid << " " << m_object_off << "~"
                         << m_object_len << dendl;
  send_pre();
}

void AbstractAioObjectWrite::send_pre() {
  bool write = false;
  {
    RWLock::RLocker snap_lock(m_ictx->snap_lock);
    if (m_ictx->object_map == nullptr) {
      m_object_exist = true;
      write = true;
    } else {
      // should have been flushed prior to releasing lock
      assert(m_ictx->exclusive_lock->is_lock_owner());
      m_object_exist = m_ictx->object_map->object_may_exist(m_object_no);

      uint8_t new_state;
      pre_object_map_update(&new_state);

      RWLock::WLocker object_map_locker(m_ictx->object_map_lock);
      if (m_ictx->object_map->update_required(m_object_no, new_state)) {
        ldout(m_ictx->cct, 20) << "send_pre " << this << " " << m_oid << " "
                               << m_object_off << "~" << m_object_len
                               << dendl;
        m_state = LIBRBD_AIO_WRITE_PRE;

        Context *ctx = util::create_context_callback<AioObjectRequest>(this);
        bool updated = m_ictx->object_map->aio_update(m_object_no, new_state,
                                                      {}, ctx);
        assert(updated);
      } else {
        write = true;
      }
    }
  }

  // avoid possible recursive lock attempts
  if (write) {
    // no object map update required
    send_write();
  }
}

bool AbstractAioObjectWrite::send_post() {
  RWLock::RLocker snap_locker(m_ictx->snap_lock);
  if (m_ictx->object_map == nullptr || !post_object_map_update()) {
    return true;
  }

  // should have been flushed prior to releasing lock
  assert(m_ictx->exclusive_lock->is_lock_owner());

  RWLock::WLocker object_map_locker(m_ictx->object_map_lock);
  if (!m_ictx->object_map->update_required(m_object_no, OBJECT_NONEXISTENT)) {
    return true;
  }

  ldout(m_ictx->cct, 20) << "send_post " << this << " " << m_oid << " "
                         << m_object_off << "~" << m_object_len << dendl;
  m_state = LIBRBD_AIO_WRITE_POST;

  Context *ctx = util::create_context_callback<AioObjectRequest>(this);
  bool updated = m_ictx->object_map->aio_update(m_object_no,
                                                OBJECT_NONEXISTENT,
      			                  OBJECT_PENDING, ctx);
  assert(updated);
  return false;
}

void AbstractAioObjectWrite::send_write() {
  ldout(m_ictx->cct, 20) << "send_write " << this << " " << m_oid << " "
      		         << m_object_off << "~" << m_object_len
                         << " object exist " << m_object_exist << dendl;

  if (!m_object_exist && has_parent()) {
    m_state = LIBRBD_AIO_WRITE_GUARD;
    handle_write_guard();
  } else {
    send_write_op(true);
  }
}

void AbstractAioObjectWrite::send_copyup()
{
  ldout(m_ictx->cct, 20) << "send_copyup " << this << " " << m_oid << " "
                         << m_object_off << "~" << m_object_len << dendl;
  m_state = LIBRBD_AIO_WRITE_COPYUP;

  m_ictx->copyup_list_lock.Lock();
  map<uint64_t, CopyupRequest*>::iterator it =
    m_ictx->copyup_list.find(m_object_no);
  if (it == m_ictx->copyup_list.end()) {
    CopyupRequest *new_req = new CopyupRequest(m_ictx, m_oid,
                                               m_object_no,
                                               std::move(m_parent_extents));
    m_parent_extents.clear();

    // make sure to wait on this CopyupRequest
    new_req->append_request(this);
    m_ictx->copyup_list[m_object_no] = new_req;

    m_ictx->copyup_list_lock.Unlock();
    new_req->send();
  } else {
    it->second->append_request(this);
    m_ictx->copyup_list_lock.Unlock();
  }
}
void AbstractAioObjectWrite::send_write_op(bool write_guard)
{
  m_state = LIBRBD_AIO_WRITE_FLAT;
  if (write_guard)
    guard_write();
  add_write_ops(&m_write);
  assert(m_write.size() != 0);

  librados::AioCompletion *rados_completion =
    util::create_rados_safe_callback(this);
  int r = m_ictx->data_ctx.aio_operate(m_oid, rados_completion, &m_write,
                                       m_snap_seq, m_snaps);
  assert(r == 0);
  rados_completion->release();
}
void AbstractAioObjectWrite::handle_write_guard()
{
  bool has_parent;
  {
    RWLock::RLocker snap_locker(m_ictx->snap_lock);
    RWLock::RLocker parent_locker(m_ictx->parent_lock);
    has_parent = compute_parent_extents();
  }
  // If parent still exists, overlap might also have changed.
  if (has_parent) {
    send_copyup();
  } else {
    // parent may have disappeared -- send original write again
    ldout(m_ictx->cct, 20) << "should_complete(" << this
                           << "): parent overlap now 0" << dendl;
    send_write();
  }
}

void AioObjectWrite::add_write_ops(librados::ObjectWriteOperation *wr) {
  RWLock::RLocker snap_locker(m_ictx->snap_lock);
  if (m_ictx->enable_alloc_hint &&
      (m_ictx->object_map == nullptr || !m_object_exist)) {
    wr->set_alloc_hint(m_ictx->get_object_size(), m_ictx->get_object_size());
  }

  if (m_object_off == 0 && m_object_len == m_ictx->get_object_size()) {
    wr->write_full(m_write_data);
  } else {
    wr->write(m_object_off, m_write_data);
  }
  wr->set_op_flags2(m_op_flags);
}

void AioObjectWrite::send_write() {
  bool write_full = (m_object_off == 0 && m_object_len == m_ictx->get_object_size());
  ldout(m_ictx->cct, 20) << "send_write " << this << " " << m_oid << " "
                         << m_object_off << "~" << m_object_len
                         << " object exist " << m_object_exist
                         << " write_full " << write_full << dendl;
  if (write_full && !has_parent()) {
    send_write_op(false);
  } else {
    AbstractAioObjectWrite::send_write();
  }
}

void AioObjectRemove::guard_write() {
  // do nothing to disable write guard only if deep-copyup not required
  RWLock::RLocker snap_locker(m_ictx->snap_lock);
  if (!m_ictx->snaps.empty()) {
    AbstractAioObjectWrite::guard_write();
  }
}
void AioObjectRemove::send_write() {
  ldout(m_ictx->cct, 20) << "send_write " << this << " " << m_oid << " "
                         << m_object_off << "~" << m_object_len << dendl;
  send_write_op(true);
}
void AioObjectTruncate::send_write() {
  ldout(m_ictx->cct, 20) << "send_write " << this << " " << m_oid
                         << " truncate " << m_object_off << dendl;
  if (!m_object_exist && ! has_parent()) {
    m_state = LIBRBD_AIO_WRITE_FLAT;
    Context *ctx = util::create_context_callback<AioObjectRequest>(this);
    m_ictx->op_work_queue->queue(ctx, 0);
  } else {
    AbstractAioObjectWrite::send_write();
  }
}

} // namespace librbd

template class librbd::AioObjectRequest<librbd::ImageCtx>;
template class librbd::AioObjectRead<librbd::ImageCtx>;
