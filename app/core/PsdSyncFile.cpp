#include "PsdSyncFile.h"

#include <QString>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mn::preview {

namespace {
namespace ps = PSD_NAMESPACE;
// What DoRead hands back and DoWaitForRead consumes — allocated through the SDK's allocator like the rest of it.
struct SyncReadOp { size_t bytesRead; size_t expected; };
}  // namespace

PsdSyncFile::PsdSyncFile(ps::Allocator* allocator) : ps::File(allocator) {}
PsdSyncFile::~PsdSyncFile() { if (m_fd >= 0) ::close(m_fd); }

bool PsdSyncFile::DoOpenRead(const wchar_t* filename) {
    const QByteArray utf8 = QString::fromWCharArray(filename).toUtf8();
    m_fd = ::open(utf8.constData(), O_RDONLY);
    return m_fd >= 0;
}
bool PsdSyncFile::DoOpenWrite(const wchar_t* filename) {   // (the test fixture writer; previews never write)
    const QByteArray utf8 = QString::fromWCharArray(filename).toUtf8();
    m_fd = ::open(utf8.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return m_fd >= 0;
}
bool PsdSyncFile::DoClose() {
    if (m_fd < 0) return true;
    const int rc = ::close(m_fd);
    m_fd = -1;
    return rc == 0;
}
ps::File::ReadOperation PsdSyncFile::DoRead(void* buffer, uint32_t count, uint64_t position) {
    ssize_t got = 0;
    if (m_fd >= 0) { const ssize_t r = ::pread(m_fd, buffer, count, off_t(position)); if (r > 0) got = r; }   // pread: no shared file position
    auto* op = static_cast<SyncReadOp*>(m_allocator->Allocate(sizeof(SyncReadOp), alignof(SyncReadOp)));
    op->bytesRead = size_t(got);
    op->expected = count;
    return op;
}
bool PsdSyncFile::DoWaitForRead(ReadOperation& operation) {
    auto* op = static_cast<SyncReadOp*>(operation);
    const bool ok = op->bytesRead == op->expected;
    m_allocator->Free(op);
    operation = nullptr;
    return ok;
}
ps::File::WriteOperation PsdSyncFile::DoWrite(const void* buffer, uint32_t count, uint64_t position) {
    ssize_t put = 0;
    if (m_fd >= 0) { const ssize_t r = ::pwrite(m_fd, buffer, count, off_t(position)); if (r > 0) put = r; }
    auto* op = static_cast<SyncReadOp*>(m_allocator->Allocate(sizeof(SyncReadOp), alignof(SyncReadOp)));
    op->bytesRead = size_t(put);
    op->expected = count;
    return op;
}
bool PsdSyncFile::DoWaitForWrite(WriteOperation& operation) {
    auto* op = static_cast<SyncReadOp*>(operation);
    const bool ok = op->bytesRead == op->expected;
    m_allocator->Free(op);
    operation = nullptr;
    return ok;
}
uint64_t PsdSyncFile::DoGetSize() const {
    if (m_fd < 0) return 0;
    struct stat st;
    return ::fstat(m_fd, &st) == 0 ? uint64_t(st.st_size) : 0;
}

}  // namespace mn::preview
