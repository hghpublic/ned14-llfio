/* handle.hpp
A handle to something
(C) 2015 Niall Douglas http://www.nedprod.com/
File Created: Dec 2015


Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "../../../handle.hpp"
#include "import.hpp"

BOOST_AFIO_V2_NAMESPACE_BEGIN

handle::handle(const handle &o, handle::really_copy)
{
  BOOST_AFIO_LOG_FUNCTION_CALL;
  _caching = o._caching;
  _flags = o._flags;
  _v.behaviour = o._v.behaviour;
  if(!DuplicateHandle(GetCurrentProcess(), o._v.h, GetCurrentProcess(), &_v.h, 0, false, DUPLICATE_SAME_ACCESS))
    throw std::system_error(GetLastError(), std::system_category());
}

handle::~handle()
{
  if(_v)
  {
    // Call close() below
    auto ret = handle::close();
    if(ret.has_error())
    {
      BOOST_AFIO_LOG_FATAL_EXIT("handle::~handle() close failed with " << ret.get_error().message());
    }
  }
}

result<void> handle::close() noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL;
  if(_v)
  {
    if(are_safety_fsyncs_issued())
    {
      if(!FlushFileBuffers(_v.h))
        return make_errored_result<void>(GetLastError());
    }
    if(!CloseHandle(_v.h))
      return make_errored_result<void>(GetLastError());
    _v = native_handle_type();
  }
  return make_result<void>();
}

result<void> handle::set_append_only(bool enable) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL;
  // This works only due to special handling in OVERLAPPED later
  if(enable)
  {
    // Set append_only
    _v.behaviour |= native_handle_type::disposition::append_only;
  }
  else
  {
    // Remove append_only
    _v.behaviour &= ~native_handle_type::disposition::append_only;
  }
  return make_result<void>();
}

result<void> handle::set_kernel_caching(caching caching) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL;
  native_handle_type nativeh;
  handle::mode _mode = mode::none;
  if(is_append_only())
    _mode = mode::append;
  else if(is_writable())
    _mode = mode::write;
  else if(is_readable())
    _mode = mode::read;
  BOOST_OUTCOME_FILTER_ERROR(access, access_mask_from_handle_mode(nativeh, _mode));
  BOOST_OUTCOME_FILTER_ERROR(attribs, attributes_from_handle_caching_and_flags(nativeh, caching, _flags));
  nativeh.behaviour |= native_handle_type::disposition::file;
  if(INVALID_HANDLE_VALUE == (nativeh.h = ReOpenFile(_v.h, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, attribs)))
    return make_errored_result<void>(GetLastError());
  _v.swap(nativeh);
  if(!CloseHandle(nativeh.h))
    return make_errored_result<void>(GetLastError());
  return make_result<void>();
}

template <class BuffersType, class Syscall> inline io_handle::io_result<BuffersType> do_read_write(const native_handle_type &nativeh, Syscall &&syscall, io_handle::io_request<BuffersType> reqs, deadline d) noexcept
{
  if(d && !nativeh.is_overlapped())
    return make_errored_result<BuffersType>(ENOTSUP);
  if(reqs.buffers.size() > 64)
    return make_errored_result<BuffersType>(E2BIG);

  BOOST_AFIO_WIN_DEADLINE_TO_SLEEP_INIT(d);
  std::array<OVERLAPPED, 64> _ols;
  span<OVERLAPPED> ols(_ols.data(), reqs.buffers.size());
  auto ol_it = ols.begin();
  DWORD transferred = 0;
  auto cancel_io = detail::Undoer([&] {
    if(nativeh.is_overlapped())
    {
      for(auto &ol : ols)
      {
        CancelIoEx(nativeh.h, &ol);
      }
      for(auto &ol : ols)
      {
        ntwait(nativeh.h, ol, deadline());
      }
    }
  });
  for(auto &req : reqs.buffers)
  {
    OVERLAPPED &ol = *ol_it++;
    ol.Internal = (ULONG_PTR) -1;
    if(nativeh.is_append_only())
      ol.OffsetHigh = ol.Offset = 0xffffffff;
    else
    {
      ol.OffsetHigh = (reqs.offset >> 32) & 0xffffffff;
      ol.Offset = reqs.offset & 0xffffffff;
    }
    if(!syscall(nativeh.h, req.first, (DWORD) req.second, &transferred, &ol) && ERROR_IO_PENDING != GetLastError())
      return make_errored_result<BuffersType>(GetLastError());
    reqs.offset += req.second;
  }
  // If handle is overlapped, wait for completion of each i/o.
  if(nativeh.is_overlapped())
  {
    for(auto &ol : ols)
    {
      deadline nd = d;
      BOOST_AFIO_WIN_DEADLINE_TO_PARTIAL_DEADLINE(nd, d);
      if(STATUS_TIMEOUT == ntwait(nativeh.h, ol, nd))
      {
        BOOST_AFIO_WIN_DEADLINE_TO_TIMEOUT(BuffersType, d);
      }
    }
  }
  cancel_io.dismiss();
  for(size_t n = 0; n < reqs.buffers.size(); n++)
  {
    if(ols[n].Internal != 0)
    {
      return make_errored_result_nt<BuffersType>((NTSTATUS) ols[n].Internal);
    }
    reqs.buffers[n].second = ols[n].InternalHigh;
  }
  return io_handle::io_result<BuffersType>(std::move(reqs.buffers));
}

io_handle::io_result<io_handle::buffers_type> io_handle::read(io_handle::io_request<io_handle::buffers_type> reqs, deadline d) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL;
  return do_read_write(_v, &ReadFile, std::move(reqs), std::move(d));
}

io_handle::io_result<io_handle::const_buffers_type> io_handle::write(io_handle::io_request<io_handle::const_buffers_type> reqs, deadline d) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL;
  return do_read_write(_v, &WriteFile, std::move(reqs), std::move(d));
}

result<io_handle::extent_guard> io_handle::lock(io_handle::extent_type offset, io_handle::extent_type bytes, bool exclusive, deadline d) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL;
  if(d && d.nsecs > 0 && !_v.is_overlapped())
    return make_errored_result<io_handle::extent_guard>(ENOTSUP);
  DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
  if(d && !d.nsecs)
    flags |= LOCKFILE_FAIL_IMMEDIATELY;
  BOOST_AFIO_WIN_DEADLINE_TO_SLEEP_INIT(d);
  OVERLAPPED ol = {0};
  ol.Internal = (ULONG_PTR) -1;
  ol.OffsetHigh = (offset >> 32) & 0xffffffff;
  ol.Offset = offset & 0xffffffff;
  DWORD bytes_high = (DWORD)((bytes >> 32) & 0xffffffff);
  DWORD bytes_low = (DWORD)(bytes & 0xffffffff);
  if(!LockFileEx(_v.h, flags, 0, bytes_low, bytes_high, &ol))
  {
    if(ERROR_LOCK_VIOLATION == GetLastError() && d && !d.nsecs)
      return make_errored_result<io_handle::extent_guard>(ETIMEDOUT);
    if(ERROR_IO_PENDING != GetLastError())
      return make_errored_result<io_handle::extent_guard>(GetLastError());
  }
  // If handle is overlapped, wait for completion of each i/o.
  if(_v.is_overlapped())
  {
    if(STATUS_TIMEOUT == ntwait(_v.h, ol, d))
    {
      BOOST_AFIO_WIN_DEADLINE_TO_TIMEOUT(io_handle::extent_guard, d);
    }
    if(ol.Internal != 0)
      return make_errored_result_nt<io_handle::extent_guard>((NTSTATUS) ol.Internal);
  }
  return extent_guard(this, offset, bytes, exclusive);
}

void io_handle::unlock(io_handle::extent_type offset, io_handle::extent_type bytes) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL;
  OVERLAPPED ol = {0};
  ol.Internal = (ULONG_PTR) -1;
  ol.OffsetHigh = (offset >> 32) & 0xffffffff;
  ol.Offset = offset & 0xffffffff;
  DWORD bytes_high = (DWORD)((bytes >> 32) & 0xffffffff);
  DWORD bytes_low = (DWORD)(bytes & 0xffffffff);
  if(!UnlockFileEx(_v.h, 0, bytes_low, bytes_high, &ol))
  {
    if(ERROR_IO_PENDING != GetLastError())
    {
      auto ret = make_errored_result<void>(GetLastError());
      BOOST_AFIO_LOG_FATAL_EXIT("io_handle::unlock() failed with " << ret.get_error().message());
      return;
    }
  }
  // If handle is overlapped, wait for completion of each i/o.
  if(_v.is_overlapped())
  {
    ntwait(_v.h, ol, deadline());
    if(ol.Internal != 0)
    {
      auto ret = make_errored_result_nt<void>((NTSTATUS) ol.Internal);
      BOOST_AFIO_LOG_FATAL_EXIT("io_handle::unlock() failed with " << ret.get_error().message());
      return;
    }
  }
}

BOOST_AFIO_V2_NAMESPACE_END
