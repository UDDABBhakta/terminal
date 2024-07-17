// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "VtIo.hpp"

#include <til/unicode.h>

#include "handle.h" // LockConsole
#include "output.h" // CloseConsoleProcessState
#include "../interactivity/inc/ServiceLocator.hpp"
#include "../renderer/base/renderer.hpp"
#include "../types/inc/CodepointWidthDetector.hpp"
#include "../types/inc/utils.hpp"

using namespace Microsoft::Console;
using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::VirtualTerminal;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::Utils;
using namespace Microsoft::Console::Interactivity;

[[nodiscard]] HRESULT VtIo::Initialize(const ConsoleArguments* const pArgs)
{
    _lookingForCursorPosition = pArgs->GetInheritCursor();

    // If we were already given VT handles, set up the VT IO engine to use those.
    if (pArgs->InConptyMode())
    {
        // Honestly, no idea where else to put this.
        if (const auto& textMeasurement = pArgs->GetTextMeasurement(); !textMeasurement.empty())
        {
            auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
            SettingsTextMeasurementMode settingsMode = SettingsTextMeasurementMode::Graphemes;
            TextMeasurementMode mode = TextMeasurementMode::Graphemes;

            if (textMeasurement == L"wcswidth")
            {
                settingsMode = SettingsTextMeasurementMode::Wcswidth;
                mode = TextMeasurementMode::Wcswidth;
            }
            else if (textMeasurement == L"console")
            {
                settingsMode = SettingsTextMeasurementMode::Console;
                mode = TextMeasurementMode::Console;
            }

            gci.SetTextMeasurementMode(settingsMode);
            CodepointWidthDetector::Singleton().Reset(mode);
        }

        return _Initialize(pArgs->GetVtInHandle(), pArgs->GetVtOutHandle(), pArgs->GetSignalHandle());
    }
    // Didn't need to initialize if we didn't have VT stuff. It's still OK, but report we did nothing.
    else
    {
        return S_FALSE;
    }
}

// Routine Description:
//  Tries to initialize this VtIo instance from the given pipe handles and
//      VtIoMode. The pipes should have been created already (by the caller of
//      conhost), in non-overlapped mode.
//  The VtIoMode string can be the empty string as a default value.
// Arguments:
//  InHandle: a valid file handle. The console will
//      read VT sequences from this pipe to generate INPUT_RECORDs and other
//      input events.
//  OutHandle: a valid file handle. The console
//      will be "rendered" to this pipe using VT sequences
//  SignalHandle: an optional file handle that will be used to send signals into the console.
//      This represents the ability to send signals to a *nix tty/pty.
// Return Value:
//  S_OK if we initialized successfully, otherwise an appropriate HRESULT
//      indicating failure.
[[nodiscard]] HRESULT VtIo::_Initialize(const HANDLE InHandle,
                                        const HANDLE OutHandle,
                                        _In_opt_ const HANDLE SignalHandle)
{
    FAIL_FAST_IF_MSG(_initialized, "Someone attempted to double-_Initialize VtIo");

    _hInput.reset(InHandle);
    _hOutput.reset(OutHandle);
    _hSignal.reset(SignalHandle);

    if (Utils::HandleWantsOverlappedIo(_hOutput.get()))
    {
        _overlappedEvent.reset(CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
        if (_overlappedEvent)
        {
            _overlappedBuf.hEvent = _overlappedEvent.get();
            _overlapped = &_overlappedBuf;
        }
    }

    // The only way we're initialized is if the args said we're in conpty mode.
    // If the args say so, then at least one of in, out, or signal was specified
    _initialized = true;
    return S_OK;
}

// Method Description:
// - Create the VtEngine and the VtInputThread for this console.
// MUST BE DONE AFTER CONSOLE IS INITIALIZED, to make sure we've gotten the
//  buffer size from the attached client application.
// Arguments:
// - <none>
// Return Value:
//  S_OK if we initialized successfully,
//  S_FALSE if VtIo hasn't been initialized (or we're not in conpty mode)
//  otherwise an appropriate HRESULT indicating failure.
[[nodiscard]] HRESULT VtIo::CreateIoHandlers() noexcept
{
    if (!_initialized)
    {
        return S_FALSE;
    }

    // SetWindowVisibility uses the console lock to protect access to _pVtRenderEngine.
    assert(ServiceLocator::LocateGlobals().getConsoleInformation().IsConsoleLocked());

    try
    {
        if (IsValidHandle(_hInput.get()))
        {
            _pVtInputThread = std::make_unique<VtInputThread>(std::move(_hInput), _lookingForCursorPosition);
        }
    }
    CATCH_RETURN();

    return S_OK;
}

bool VtIo::IsUsingVt() const
{
    return _initialized;
}

// Routine Description:
//  Potentially starts this VtIo's input thread and render engine.
//      If the VtIo hasn't yet been given pipes, then this function will
//      silently do nothing. It's the responsibility of the caller to make sure
//      that the pipes are initialized first with VtIo::Initialize
// Arguments:
//  <none>
// Return Value:
//  S_OK if we started successfully or had nothing to start, otherwise an
//      appropriate HRESULT indicating failure.
[[nodiscard]] HRESULT VtIo::StartIfNeeded()
{
    // If we haven't been set up, do nothing (because there's nothing to start)
    if (!_initialized)
    {
        return S_FALSE;
    }

    if (_pVtInputThread)
    {
        LOG_IF_FAILED(_pVtInputThread->Start());
    }

    {
        auto writer = GetWriter();

        // GH#4999 - Send a sequence to the connected terminal to request
        // win32-input-mode from them. This will enable the connected terminal to
        // send us full INPUT_RECORDs as input. If the terminal doesn't understand
        // this sequence, it'll just ignore it.

        writer.WriteUTF8(
            "\033[?1004h" // Focus Event Mode
            "\033[?9001h" // Win32 Input Mode
        );

        // MSFT: 15813316
        // If the terminal application wants us to inherit the cursor position,
        //  we're going to emit a VT sequence to ask for the cursor position, then
        //  wait 1s until we get a response.
        // If we get a response, the InteractDispatch will call SetCursorPosition,
        //      which will call to our VtIo::SetCursorPosition method.
        if (_lookingForCursorPosition)
        {
            writer.WriteUTF8("\x1b[6n"); // Cursor Position Report (DSR CPR)
        }

        writer.Submit();
    }

    if (_lookingForCursorPosition)
    {
        _lookingForCursorPosition = false;

        // Allow the input thread to momentarily gain the console lock.
        const auto suspension = g.getConsoleInformation().SuspendLock();
        _pVtInputThread->WaitUntilDSR(3000);
    }

    if (_pPtySignalInputThread)
    {
        // Let the signal thread know that the console is connected.
        //
        // By this point, the pseudo window should have already been created, by
        // ConsoleInputThreadProcWin32. That thread has a message pump, which is
        // needed to ensure that DPI change messages to the owning terminal
        // window don't end up hanging because the pty didn't also process it.
        _pPtySignalInputThread->ConnectConsole();
    }

    return S_OK;
}

// Method Description:
// - Create our pseudo window. This is exclusively called by
//   ConsoleInputThreadProcWin32 on the console input thread.
//    * It needs to be called on that thread, before any other calls to
//      LocatePseudoWindow, to make sure that the input thread is the HWND's
//      message thread.
//    * It needs to be plumbed through the signal thread, because the signal
//      thread knows if someone should be marked as the window's owner. It's
//      VERY IMPORTANT that any initial owners are set up when the window is
//      first created.
// - Refer to GH#13066 for details.
void VtIo::CreatePseudoWindow()
{
    if (_pPtySignalInputThread)
    {
        _pPtySignalInputThread->CreatePseudoWindow();
    }
    else
    {
        ServiceLocator::LocatePseudoWindow();
    }
}

// Method Description:
// - Create and start the signal thread. The signal thread can be created
//      independent of the i/o threads, and doesn't require a client first
//      attaching to the console. We need to create it first and foremost,
//      because it's possible that a terminal application could
//      CreatePseudoConsole, then ClosePseudoConsole without ever attaching a
//      client. Should that happen, we still need to exit.
// Arguments:
// - <none>
// Return Value:
// - S_FALSE if we're not in VtIo mode,
//   S_OK if we succeeded,
//   otherwise an appropriate HRESULT indicating failure.
[[nodiscard]] HRESULT VtIo::CreateAndStartSignalThread() noexcept
{
    if (!_initialized)
    {
        return S_FALSE;
    }

    // If we were passed a signal handle, try to open it and make a signal reading thread.
    if (IsValidHandle(_hSignal.get()))
    {
        try
        {
            _pPtySignalInputThread = std::make_unique<PtySignalInputThread>(std::move(_hSignal));

            // Start it if it was successfully created.
            RETURN_IF_FAILED(_pPtySignalInputThread->Start());
        }
        CATCH_RETURN();
    }

    return S_OK;
}

void VtIo::SendCloseEvent()
{
    LockConsole();
    const auto unlock = wil::scope_exit([] { UnlockConsole(); });

    // This function is called when the ConPTY signal pipe is closed (PtySignalInputThread) and when the input
    // pipe is closed (VtIo). Usually these two happen at about the same time. This if condition is a bit of
    // a premature optimization and prevents us from sending out a CTRL_CLOSE_EVENT right after another.
    if (!std::exchange(_closeEventSent, true))
    {
        CloseConsoleProcessState();
    }
}

// Returns true for C0 characters and C1 [single-character] CSI.
// A copy of isActionableFromGround() from stateMachine.cpp.
static constexpr bool IsControlCharacter(wchar_t wch) noexcept
{
    // This is equivalent to:
    //   return (wch <= 0x1f) || (wch >= 0x7f && wch <= 0x9f);
    // It's written like this to get MSVC to emit optimal assembly for findActionableFromGround.
    // It lacks the ability to turn boolean operators into binary operations and also happens
    // to fail to optimize the printable-ASCII range check into a subtraction & comparison.
    return (wch <= 0x1f) | (static_cast<wchar_t>(wch - 0x7f) <= 0x20);
}

// Formats the given console attributes to their closest VT equivalent.
// `out` must refer to at least `formatAttributesMaxLen` characters of valid memory.
// Returns a pointer past the end.
static constexpr size_t formatAttributesMaxLen = 16;
static char* formatAttributes(char* out, const TextAttribute& attributes) noexcept
{
    static uint8_t sgr[] = { 30, 31, 32, 33, 34, 35, 36, 37, 90, 91, 92, 93, 94, 95, 96, 97 };

    // Applications expect that SetConsoleTextAttribute() completely replaces whatever attributes are currently set,
    // including any potential VT-exclusive attributes. Since we don't know what those are, we must always emit a SGR 0.
    // Copying 4 bytes instead of the correct 3 means we need just 1 DWORD mov. Neat.
    //
    // 3 bytes.
    memcpy(out, "\x1b[0", 4);
    out += 3;

    // 2 bytes.
    if (attributes.IsReverseVideo())
    {
        memcpy(out, ";7", 2);
        out += 2;
    }

    // 3 bytes (";97").
    if (attributes.GetForeground().IsLegacy())
    {
        const uint8_t index = sgr[attributes.GetForeground().GetIndex()];
        out = fmt::format_to(out, FMT_COMPILE(";{}"), index);
    }

    // 4 bytes (";107").
    if (attributes.GetBackground().IsLegacy())
    {
        const uint8_t index = sgr[attributes.GetBackground().GetIndex()] + 10;
        out = fmt::format_to(out, FMT_COMPILE(";{}"), index);
    }

    // 1 byte.
    *out++ = 'm';
    return out;
}

void VtIo::FormatAttributes(std::string& target, const TextAttribute& attributes)
{
    char buf[formatAttributesMaxLen];
    const size_t len = formatAttributes(&buf[0], attributes) - &buf[0];
    target.append(buf, len);
}

void VtIo::FormatAttributes(std::wstring& target, const TextAttribute& attributes)
{
    char buf[formatAttributesMaxLen];
    const size_t len = formatAttributes(&buf[0], attributes) - &buf[0];

    wchar_t bufW[formatAttributesMaxLen];
    for (size_t i = 0; i < len; i++)
    {
        bufW[i] = buf[i];
    }

    target.append(bufW, len);
}

VtIo::Writer VtIo::GetWriter() noexcept
{
    _corked += 1;
    return Writer{ this };
}

VtIo::Writer::Writer(VtIo* io) noexcept :
    _io{ io }
{
}

VtIo::Writer::~Writer() noexcept
{
    // If _io is non-null, then we didn't call Submit, e.g. because of an exception.
    // We need to avoid flushing the buffer in that case.
    if (_io)
    {
        _io->_writerTainted = true;
        _io->_uncork();
    }
}

VtIo::Writer::Writer(Writer&& other) noexcept :
    _io{ std::exchange(other._io, nullptr) }
{
}

VtIo::Writer& VtIo::Writer::operator=(Writer&& other) noexcept
{
    if (this != &other)
    {
        this->~Writer();
        _io = std::exchange(other._io, nullptr);
    }
    return *this;
}

VtIo::Writer::operator bool() const noexcept
{
    return _io != nullptr;
}

void VtIo::Writer::Submit()
{
    const auto io = std::exchange(_io, nullptr);
    io->_uncork();
}

void VtIo::_uncork()
{
    _corked -= 1;
    if (_corked <= 0)
    {
        _flushNow();
    }
}

void VtIo::_flushNow()
{
    size_t minSize = 0;

    if (_writerRestoreCursor)
    {
        minSize = 4;
        _writerRestoreCursor = false;
        _back.append("\x1b\x38"); // DECRC: DEC Restore Cursor (+ attributes)
    }

    if (_overlappedPending)
    {
        _overlappedPending = false;

        DWORD written;
        if (FAILED(Utils::GetOverlappedResultSameThread(_overlapped, &written)))
        {
            // Not much we can do here. Let's treat this like a ERROR_BROKEN_PIPE.
            _hOutput.reset();
            SendCloseEvent();
        }
    }

    _front.clear();
    _front.swap(_back);

    // If it's >128KiB large and twice as large as the previous buffer, free the memory.
    // This ensures that there's a pathway for shrinking the buffer from large sizes.
    if (const auto cap = _back.capacity(); cap > 128 * 1024 && cap / 2 > _front.size())
    {
        _back = std::string{};
    }

    // We encountered an exception and shouldn't flush the broken pieces.
    if (_writerTainted)
    {
        _writerTainted = false;
        return;
    }

    // If _back (now _front) was empty, we can return early. If all _front contains is
    // DECSC/DECRC that was added by BackupCursor & us, we can also return early.
    if (_front.size() <= minSize)
    {
        return;
    }

    // No point in calling WriteFile if we already encountered ERROR_BROKEN_PIPE.
    // We do this after the above, so that _back doesn't grow indefinitely.
    if (!_hOutput)
    {
        return;
    }

    const auto write = gsl::narrow_cast<DWORD>(_front.size());

    TraceLoggingWrite(
        g_hConhostV2EventTraceProvider,
        "ConPTY WriteFile",
        TraceLoggingCountedUtf8String(_front.data(), write, "buffer"),
        TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
        TraceLoggingKeyword(TIL_KEYWORD_TRACE));

    for (;;)
    {
        if (WriteFile(_hOutput.get(), _front.data(), write, nullptr, _overlapped))
        {
            return;
        }

        switch (const auto gle = GetLastError())
        {
        case ERROR_BROKEN_PIPE:
            _hOutput.reset();
            SendCloseEvent();
            return;
        case ERROR_IO_PENDING:
            _overlappedPending = true;
            return;
        default:
            LOG_WIN32(gle);
            return;
        }
    }
}

void VtIo::Writer::BackupCursor() const
{
    if (!_io->_writerRestoreCursor)
    {
        _io->_writerRestoreCursor = true;
        _io->_back.append("\x1b\x37"); // DECSC: DEC Save Cursor (+ attributes)
    }
}

void VtIo::Writer::WriteUTF8(std::string_view str) const
{
    _io->_back.append(str);
}

void VtIo::Writer::WriteUTF16(std::wstring_view str) const
{
    if (str.empty())
    {
        return;
    }

    const auto existingUTF8Len = _io->_back.size();
    const auto incomingUTF16Len = str.size();

    // When converting from UTF-16 to UTF-8 the worst case is 3 bytes per UTF-16 code unit.
    const auto incomingUTF8Cap = incomingUTF16Len * 3;
    const auto totalUTF8Cap = existingUTF8Len + incomingUTF8Cap;

    // Since WideCharToMultiByte() only supports `int` lengths, we check for an overflow past INT_MAX/3.
    // We also check for an overflow of totalUTF8Cap just to be sure.
    if (incomingUTF16Len > gsl::narrow_cast<size_t>(INT_MAX / 3) || totalUTF8Cap <= existingUTF8Len)
    {
        THROW_HR_MSG(E_INVALIDARG, "string too large");
    }

    // NOTE: Throwing inside resize_and_overwrite invokes undefined behavior.
    _io->_back._Resize_and_overwrite(totalUTF8Cap, [&](char* buf, const size_t) noexcept {
        const auto len = WideCharToMultiByte(CP_UTF8, 0, str.data(), gsl::narrow_cast<int>(incomingUTF16Len), buf + existingUTF8Len, gsl::narrow_cast<int>(incomingUTF8Cap), nullptr, nullptr);
        return existingUTF8Len + std::max(0, len);
    });
}

// When DISABLE_NEWLINE_AUTO_RETURN is not set (Bad! Don't do it!) we'll do newline translation for you.
// That's the only difference of this function from WriteUTF16: It does LF -> CRLF translation.
void VtIo::Writer::WriteUTF16TranslateCRLF(std::wstring_view str) const
{
    const auto beg = str.begin();
    const auto end = str.end();
    auto begCopy = beg;
    auto endCopy = beg;

    // Our goal is to prepend a \r in front of \n that don't already have one.
    // There's no point in replacing \n\n\n with \r\n\r\n\r\n, however. It's just fine to do \r\n\n\n.
    // After all we aren't a text file, we're a terminal, and \r\n and \n are identical if we're at the first column.
    for (;;)
    {
        // To do so, we'll first find the next LF and emit the unrelated text before it.
        endCopy = std::find(endCopy, end, L'\n');
        WriteUTF16({ begCopy, endCopy });
        begCopy = endCopy;

        // Done? Great.
        if (begCopy == end)
        {
            break;
        }

        // We only need to prepend a CR if the LF isn't already preceded by one.
        if (begCopy == beg || begCopy[-1] != L'\r')
        {
            _io->_back.push_back('\r');
        }

        // Now extend the end of the next WriteUTF16 *past* this series of CRs and LFs.
        // We've just ensured that the LF is preceded by a CR, so we can skip all this safely.
        while (++endCopy != end && (*endCopy == L'\n' || *endCopy == L'\r'))
        {
        }
    }
}

// Same as WriteUTF16, but replaces control characters with spaces.
// We don't outright remove them because that would mess up the cursor position.
// conhost traditionally assigned control chars a width of 1 when in the raw write mode.
void VtIo::Writer::WriteUTF16StripControlChars(std::wstring_view str) const
{
    auto it = str.data();
    const auto end = it + str.size();

    // We can picture `str` as a repeated sequence of regular characters followed by control characters.
    while (it != end)
    {
        const auto begControlChars = FindActionableControlCharacter(it, end - it);

        WriteUTF16({ it, begControlChars });

        for (it = begControlChars; it != end && IsControlCharacter(*it); ++it)
        {
            WriteUCS2StripControlChars(*it);
        }
    }
}

void VtIo::Writer::WriteUCS2(wchar_t ch) const
{
    char buf[4];
    size_t len = 0;

    if (til::is_surrogate(ch))
    {
        ch = UNICODE_REPLACEMENT;
    }

    if (ch <= 0x7f)
    {
        buf[len++] = static_cast<char>(ch);
    }
    else if (ch <= 0x7ff)
    {
        buf[len++] = static_cast<char>(0xc0 | (ch >> 6));
        buf[len++] = static_cast<char>(0x80 | (ch & 0x3f));
    }
    else
    {
        buf[len++] = static_cast<char>(0xe0 | (ch >> 12));
        buf[len++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3f));
        buf[len++] = static_cast<char>(0x80 | (ch & 0x3f));
    }

    _io->_back.append(buf, len);
}

void VtIo::Writer::WriteUCS2StripControlChars(wchar_t ch) const
{
    if (ch < 0x20)
    {
        static constexpr wchar_t lut[] = {
            // clang-format off
            L' ', L'☺', L'☻', L'♥', L'♦', L'♣', L'♠', L'•', L'◘', L'○', L'◙', L'♂', L'♀', L'♪', L'♫', L'☼',
            L'►', L'◄', L'↕', L'‼', L'¶', L'§', L'▬', L'↨', L'↑', L'↓', L'→', L'←', L'∟', L'↔', L'▲', L'▼',
            // clang-format on
        };
        ch = lut[ch];
    }
    else if (ch == 0x7F)
    {
        ch = L'⌂';
    }
    else if (ch > 0x7F && ch < 0xA0)
    {
        ch = L'?';
    }

    WriteUCS2(ch);
}

// CUP: Cursor Position
void VtIo::Writer::WriteCUP(til::point position) const
{
    fmt::format_to(std::back_inserter(_io->_back), FMT_COMPILE("\x1b[{};{}H"), position.y + 1, position.x + 1);
}

// DECTCEM: Text Cursor Enable
void VtIo::Writer::WriteDECTCEM(bool enabled) const
{
    char buf[] = "\x1b[?25h";
    buf[std::size(buf) - 2] = enabled ? 'h' : 'l';
    _io->_back.append(&buf[0], std::size(buf) - 1);
}

// SGR 1006: SGR Extended Mouse Mode
void VtIo::Writer::WriteSGR1006(bool enabled) const
{
    char buf[] = "\x1b[?1003;1006h";
    buf[std::size(buf) - 2] = enabled ? 'h' : 'l';
    _io->_back.append(&buf[0], std::size(buf) - 1);
}

// DECAWM: Autowrap Mode
void VtIo::Writer::WriteDECAWM(bool enabled) const
{
    char buf[] = "\x1b[?7h";
    buf[std::size(buf) - 2] = enabled ? 'h' : 'l';
    _io->_back.append(&buf[0], std::size(buf) - 1);
}

// ASB: Alternate Screen Buffer
void VtIo::Writer::WriteASB(bool enabled) const
{
    char buf[] = "\x1b[?1049h";
    buf[std::size(buf) - 2] = enabled ? 'h' : 'l';
    _io->_back.append(&buf[0], std::size(buf) - 1);
}

void VtIo::Writer::WriteAttributes(const TextAttribute& attributes) const
{
    FormatAttributes(_io->_back, attributes);
}

void VtIo::Writer::WriteInfos(til::point target, std::span<const CHAR_INFO> infos) const
{
    const auto beg = infos.begin();
    const auto end = infos.end();
    const auto last = end - 1;
    WORD attributes = 0xffff;

    WriteCUP(target);

    for (auto it = beg; it != end; ++it)
    {
        const auto& ci = *it;
        auto ch = ci.Char.UnicodeChar;
        auto wide = WI_IsAnyFlagSet(ci.Attributes, COMMON_LVB_LEADING_BYTE | COMMON_LVB_TRAILING_BYTE);

        if (wide)
        {
            if (WI_IsAnyFlagSet(ci.Attributes, COMMON_LVB_LEADING_BYTE))
            {
                if (it == last)
                {
                    // The leading half of a wide glyph won't fit into the last remaining column.
                    // --> Replace it with a space.
                    ch = L' ';
                    wide = false;
                }
            }
            else
            {
                if (it == beg)
                {
                    // The trailing half of a wide glyph won't fit into the first column. It's incomplete.
                    // --> Replace it with a space.
                    ch = L' ';
                    wide = false;
                }
                else
                {
                    // Trailing halves of glyphs are ignored within the run. We only emit the leading half.
                    continue;
                }
            }
        }

        if (attributes != ci.Attributes)
        {
            attributes = ci.Attributes;
            WriteAttributes(TextAttribute{ attributes });
        }

        int repeat = 1;
        if (wide && (til::is_surrogate(ch) || IsControlCharacter(ch)))
        {
            // Control characters, U+FFFD, etc. are narrow characters, so if the caller
            // asked for a wide glyph we need to repeat the replacement character twice.
            repeat++;
        }

        do
        {
            WriteUCS2StripControlChars(ch);
        } while (--repeat);
    }
}
