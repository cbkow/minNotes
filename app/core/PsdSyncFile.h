#pragma once
// A synchronous ps::File for psd_sdk on POSIX (macOS / Linux), ported from ufb's SyncPsdFile (2026-10-21): the SDK's own
// NativeFile_Mac.mm dispatches every read onto a GCD queue and can run a freed block under churn (EXC_BAD_ACCESS). We
// render one PSD at a time; plain pread() is all that's needed. Windows keeps ps::NativeFile (overlapped I/O, no UAF).
// Writing (pwrite) exists only so the test suite can author a PSD fixture through the SDK's exporter.
#include "Psd.h"
#include "PsdAllocator.h"
#include "PsdFile.h"

namespace mn::preview {

class PsdSyncFile final : public PSD_NAMESPACE::File {
public:
    explicit PsdSyncFile(PSD_NAMESPACE::Allocator* allocator);
    ~PsdSyncFile() override;

private:
    bool DoOpenRead(const wchar_t* filename) override;
    bool DoOpenWrite(const wchar_t* filename) override;
    bool DoClose() override;
    ReadOperation DoRead(void* buffer, uint32_t count, uint64_t position) override;
    bool DoWaitForRead(ReadOperation& operation) override;
    WriteOperation DoWrite(const void* buffer, uint32_t count, uint64_t position) override;
    bool DoWaitForWrite(WriteOperation& operation) override;
    uint64_t DoGetSize() const override;

    int m_fd = -1;
};

}  // namespace mn::preview
