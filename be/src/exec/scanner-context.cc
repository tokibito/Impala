// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/scanner-context.h"

#include "exec/hdfs-scan-node.h"
#include "runtime/row-batch.h"
#include "runtime/mem-pool.h"
#include "runtime/runtime-state.h"
#include "runtime/string-buffer.h"
#include "util/debug-util.h"

using namespace boost;
using namespace impala;
using namespace std;

static const int DEFAULT_READ_PAST_SIZE = 1024; // in bytes

// We always want output_buffer_bytes_left_ to be non-NULL, so we can avoid a NULL check
// in GetBytes(). We use this variable, which is set to 0, to initialize
// output_buffer_bytes_left_. After the first successful call to GetBytes(),
// output_buffer_bytes_left_ will be set to something else.
static const int OUTPUT_BUFFER_BYTES_LEFT_INIT = 0;

ScannerContext::ScannerContext(RuntimeState* state, HdfsScanNode* scan_node,
    HdfsPartitionDescriptor* partition_desc, DiskIoMgr::ScanRange* scan_range)
  : state_(state),
    scan_node_(scan_node),
    partition_desc_(partition_desc),
    num_completed_io_buffers_(0) {
  AddStream(scan_range);
}

void ScannerContext::AttachCompletedResources(RowBatch* batch, bool done) {
  DCHECK(batch != NULL);
  for (int i = 0; i < streams_.size(); ++i) {
    streams_[i]->AttachCompletedResources(batch, done);
  }
  if (done) streams_.clear();
}

ScannerContext::Stream::Stream(ScannerContext* parent)
  : parent_(parent),
    boundary_pool_(new MemPool(parent->scan_node_->mem_tracker())),
    boundary_buffer_(new StringBuffer(boundary_pool_.get())) {
}

ScannerContext::Stream* ScannerContext::AddStream(DiskIoMgr::ScanRange* range) {
  Stream* stream = state_->obj_pool()->Add(new Stream(this));
  stream->scan_range_ = range;
  stream->file_desc_ = scan_node_->GetFileDesc(stream->filename());
  stream->total_bytes_returned_ = 0;
  stream->io_buffer_pos_ = NULL;
  stream->io_buffer_ = NULL;
  stream->io_buffer_bytes_left_ = 0;
  stream->boundary_buffer_bytes_left_ = 0;
  stream->output_buffer_pos_ = NULL;
  stream->output_buffer_bytes_left_ = const_cast<int*>(&OUTPUT_BUFFER_BYTES_LEFT_INIT);
  stream->contains_tuple_data_ = !scan_node_->tuple_desc()->string_slots().empty();
  streams_.push_back(stream);
  return stream;
}

void ScannerContext::Stream::AttachCompletedResources(RowBatch* batch, bool done) {
  DCHECK(batch != NULL);
  if (done) {
    // Mark any pending resources as completed
    if (io_buffer_ != NULL) {
      ++parent_->num_completed_io_buffers_;
      completed_io_buffers_.push_back(io_buffer_);
    }
    // Set variables to NULL to make sure streams are not used again
    io_buffer_ = NULL;
    io_buffer_pos_ = NULL;
    io_buffer_bytes_left_ = 0;
    // Cancel the underlying scan range to clean up any queued buffers there
    scan_range_->Cancel(Status::CANCELLED);
  }

  for (list<DiskIoMgr::BufferDescriptor*>::iterator it = completed_io_buffers_.begin();
       it != completed_io_buffers_.end(); ++it) {
    if (contains_tuple_data_) {
      batch->AddIoBuffer(*it);
      // TODO: We can do row batch compaction here.  This is the only place io buffers are
      // queued.  A good heuristic is to check the number of io buffers queued and if
      // there are too many, we should compact.
    } else {
      (*it)->Return();
      --parent_->scan_node_->num_owned_io_buffers_;
    }
  }
  parent_->num_completed_io_buffers_ -= completed_io_buffers_.size();
  completed_io_buffers_.clear();

  if (contains_tuple_data_) {
    // If we're not done, keep using the last chunk allocated in boundary_pool_ so we
    // don't have to reallocate. If we are done, transfer it to the row batch.
    batch->tuple_data_pool()->AcquireData(boundary_pool_.get(), /* keep_current */ !done);
  }
  if (done) boundary_pool_->FreeAll();
}

Status ScannerContext::Stream::GetNextBuffer(int read_past_size) {
  if (parent_->cancelled()) return Status::CANCELLED;

  // io_buffer_ should only be null the first time this is called
  DCHECK(io_buffer_ != NULL ||
         (total_bytes_returned_ == 0 && completed_io_buffers_.empty()));

  // We can't use the eosr() function because it reflects how many bytes have been
  // returned, not if we're fetched all the buffers in the scan range
  bool eosr = false;
  if (io_buffer_ != NULL) {
    eosr = io_buffer_->eosr();
    ++parent_->num_completed_io_buffers_;
    completed_io_buffers_.push_back(io_buffer_);
    io_buffer_ = NULL;
  }

  if (!eosr) {
    SCOPED_TIMER(parent_->state_->total_storage_wait_timer());
    RETURN_IF_ERROR(scan_range_->GetNext(&io_buffer_));
  } else {
    SCOPED_TIMER(parent_->state_->total_storage_wait_timer());
    int64_t offset = file_offset() + boundary_buffer_bytes_left_;

    int read_past_buffer_size = read_past_size_cb_.empty() ?
                                DEFAULT_READ_PAST_SIZE : read_past_size_cb_(offset);
    read_past_buffer_size = ::max(read_past_buffer_size, read_past_size);

    // TODO: we're reading past this scan range so this is likely a remote read.
    // Update when the IoMgr has better support for remote reads.
    DiskIoMgr::ScanRange* range = parent_->scan_node_->AllocateScanRange(
        filename(), read_past_buffer_size, offset, -1, scan_range_->disk_id(), false);
    RETURN_IF_ERROR(parent_->state_->io_mgr()->Read(
        parent_->scan_node_->reader_context(), range, &io_buffer_));
  }

  DCHECK(io_buffer_ != NULL);
  ++parent_->scan_node_->num_owned_io_buffers_;
  io_buffer_pos_ = reinterpret_cast<uint8_t*>(io_buffer_->buffer());
  io_buffer_bytes_left_ = io_buffer_->len();

  return Status::OK;
}

Status ScannerContext::Stream::GetBuffer(bool peek, uint8_t** out_buffer, int* len) {
  *out_buffer = NULL;
  *len = 0;
  if (eosr()) return Status::OK;

  if (parent_->cancelled()) {
    DCHECK(*out_buffer == NULL);
    return Status::CANCELLED;
  }

  if (boundary_buffer_bytes_left_ > 0) {
    DCHECK_EQ(output_buffer_pos_, &boundary_buffer_pos_);
    DCHECK_EQ(output_buffer_bytes_left_, &boundary_buffer_bytes_left_);
    *out_buffer = boundary_buffer_pos_;
    // Don't return more bytes past eosr
    *len = min(static_cast<int64_t>(boundary_buffer_bytes_left_), bytes_left());
    DCHECK_GE(*len, 0);
    if (!peek) {
      boundary_buffer_pos_ += *len;
      boundary_buffer_bytes_left_ -= *len;
      total_bytes_returned_ += *len;
    }
    return Status::OK;
  }

  if (io_buffer_bytes_left_ == 0) {
    // We're at the end of the boundary buffer and the current IO buffer. Get a new IO
    // buffer and set the current buffer to it.
    RETURN_IF_ERROR(GetNextBuffer());
    output_buffer_pos_ = &io_buffer_pos_;
    output_buffer_bytes_left_ = &io_buffer_bytes_left_;
  }
  DCHECK(io_buffer_ != NULL);

  *out_buffer = io_buffer_pos_;
  *len = io_buffer_bytes_left_;
  if (!peek) {
    io_buffer_bytes_left_ = 0;
    io_buffer_pos_ += *len;
    total_bytes_returned_ += *len;
  }
  DCHECK_GE(bytes_left(), 0);
  return Status::OK;
}

Status ScannerContext::Stream::GetBytesInternal(
    int requested_len, uint8_t** out_buffer, bool peek, int* out_len) {
  DCHECK_GT(requested_len, boundary_buffer_bytes_left_);
  *out_buffer = NULL;

  if (boundary_buffer_bytes_left_ == 0) {
    if (contains_tuple_data_) {
      boundary_buffer_->Reset();
    } else {
      boundary_buffer_->Clear();
    }
  }

  while (requested_len > boundary_buffer_bytes_left_ + io_buffer_bytes_left_) {
    // We need to fetch more bytes. Copy the end of the current buffer and fetch the next
    // one.
    boundary_buffer_->Append(io_buffer_pos_, io_buffer_bytes_left_);
    boundary_buffer_bytes_left_ += io_buffer_bytes_left_;

    RETURN_IF_ERROR(GetNextBuffer());
    if (parent_->cancelled()) return Status::CANCELLED;

    if (io_buffer_bytes_left_ == 0) {
      // No more bytes (i.e. EOF)
      break;
    }
  }

  // We have enough bytes in io_buffer_ or couldn't read more bytes
  int requested_bytes_left = requested_len - boundary_buffer_bytes_left_;
  DCHECK_GE(requested_len, 0);
  int num_bytes = min(io_buffer_bytes_left_, requested_bytes_left);
  *out_len = boundary_buffer_bytes_left_ + num_bytes;
  DCHECK_LE(*out_len, requested_len);

  if (boundary_buffer_bytes_left_ == 0) {
    // No stitching, just return the memory
    output_buffer_pos_ = &io_buffer_pos_;
    output_buffer_bytes_left_ = &io_buffer_bytes_left_;
  } else {
    boundary_buffer_->Append(io_buffer_pos_, num_bytes);
    boundary_buffer_bytes_left_ += num_bytes;
    boundary_buffer_pos_ = reinterpret_cast<uint8_t*>(boundary_buffer_->str().ptr) +
                           boundary_buffer_->Size() - boundary_buffer_bytes_left_;
    io_buffer_bytes_left_ -= num_bytes;
    io_buffer_pos_ += num_bytes;

    output_buffer_pos_ = &boundary_buffer_pos_;
    output_buffer_bytes_left_ = &boundary_buffer_bytes_left_;
  }
  *out_buffer = *output_buffer_pos_;

  if (!peek) {
    total_bytes_returned_ += *out_len;
    if (boundary_buffer_bytes_left_ == 0) {
      io_buffer_bytes_left_ -= num_bytes;
      io_buffer_pos_ += num_bytes;
    } else {
      DCHECK_EQ(boundary_buffer_bytes_left_, *out_len);
      boundary_buffer_bytes_left_ = 0;
    }
  }

  return Status::OK;
}

bool ScannerContext::cancelled() const {
  return scan_node_->done_;
}

Status ScannerContext::Stream::ReportIncompleteRead(int length, int bytes_read) {
  stringstream ss;
  ss << "Tried to read " << length << " bytes but could only read "
     << bytes_read << " bytes. This may indicate data file corruption. "
     << "(file: " << filename() << ", byte offset: " << file_offset() << ")";
  return Status(ss.str());
}

Status ScannerContext::Stream::ReportInvalidRead(int length) {
  stringstream ss;
  ss << "Invalid read of " << length << " bytes. This may indicate data file corruption. "
     << "(file: " << filename() << ", byte offset: " << file_offset() << ")";
  return Status(ss.str());
}
