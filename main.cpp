/**
 * @file main.cpp
 * @brief colourbench - Terminal colour output benchmark.
 * @author Leonard Hecker <leonard@hecker.io>
 * @author Alfie Ardinata <alfieardinata@outlook.com>
 *
 * @copyright Copyright (c) 2022 Leonard Hecker
 * @copyright Copyright (c) 2026 Alfie Ardinata
 *
 * @details Originally written by Leonard Hecker.
 *          Modernized and rewritten by Alfie Ardinata using C++23.
 *
 * @par License
 * SPDX-License-Identifier: MIT
 */

#include "scope.hpp"

#ifndef _WIN32
#error "colourbench only supports Windows."
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <print>
#include <ranges>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace colourbench {

using namespace std::literals;
using Clock = std::chrono::steady_clock;

inline constexpr std::string_view begin_screen { "\x1b[?1049h\x1b[?25l" };
inline constexpr std::string_view end_screen { "\x1b[?2026l\x1b[?25h\x1b[?1049l" };
inline constexpr std::string_view begin_frame { "\x1b[?2026h\x1b[H\x1b[39;49m" };
inline constexpr std::string_view end_frame { "\x1b[?2026l" };

inline constexpr std::size_t estimated_bytes_per_cell { 48 };
inline constexpr std::size_t frame_sequence_margin { 128 };

inline constexpr auto idle_poll_interval = std::chrono::milliseconds { 50 };

struct Error final {
    std::string message {};
};

template <typename T> using Result = std::expected<T, Error>;

[[nodiscard]] inline auto fail(std::string message) -> std::unexpected<Error> {
    return std::unexpected { Error { std::move(message) } };
}

struct ScreenSize final {
    std::size_t columns {};
    std::size_t rows {};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool { return columns != 0 && rows != 0; }
};

enum class ColorMode : std::uint8_t {
    all,
    foreground,
    background,
    none,
};

struct ColorFlag final {
    std::string_view name;
    ColorMode mode;
};

inline constexpr std::array color_flags {
    ColorFlag { "-ng"sv, ColorMode::none },
    ColorFlag { "-fg"sv, ColorMode::foreground },
    ColorFlag { "-bg"sv, ColorMode::background },
};

inline constexpr std::string_view glyph_prefix { "-ch=" };

struct Rgb final {
    std::uint8_t red {};
    std::uint8_t green {};
    std::uint8_t blue {};
};

struct Glyph final {
    std::array<char, 4> bytes {};
    std::uint8_t length {};

    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view { return { bytes.data(), length }; }
};

class ColorCount final {
public:
    static constexpr std::size_t maximum { 1530 };

    constexpr ColorCount() noexcept = default;

    [[nodiscard]] static auto make(const std::size_t value) -> Result<ColorCount> {
        if (value < 1 || value > maximum) { return fail(std::format("{} is outside [1, {}]", value, maximum)); }
        return ColorCount { value };
    }

    [[nodiscard]] constexpr auto value() const noexcept -> std::size_t { return value_; }

private:
    explicit constexpr ColorCount(const std::size_t value) noexcept
        : value_ { value } {}

    std::size_t value_ { maximum };
};

struct Options final {
    ColorCount color_count {};
    ColorMode color_mode { ColorMode::all };
    std::optional<Glyph> glyph {};
};

struct DecimalByte final {
    std::array<char, 3> characters {};
    std::uint8_t length {};

    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view { return { characters.data(), length }; }
};

[[nodiscard]] constexpr auto digit_count(const std::uint32_t value) noexcept -> int {
    if (value >= 100U) { return 3; }
    if (value >= 10U) { return 2; }
    return 1;
}

[[nodiscard]] constexpr auto make_decimal_byte(const std::uint32_t value) noexcept -> DecimalByte {
    DecimalByte decimal {};
    decimal.length = static_cast<std::uint8_t>(digit_count(value));

    auto remaining = value;
    for (auto& character : decimal.characters | std::views::take(decimal.length) | std::views::reverse) {
        character = static_cast<char>('0' + remaining % 10U);
        remaining /= 10U;
    }
    return decimal;
}

[[nodiscard]] consteval auto make_decimal_byte_table() -> std::array<DecimalByte, 256> {
    std::array<DecimalByte, 256> table {};
    std::ranges::copy(std::views::iota(0U, 256U) | std::views::transform(make_decimal_byte), table.begin());
    return table;
}

inline constexpr auto decimal_byte_table = make_decimal_byte_table();

[[nodiscard]] constexpr auto color_at(const std::size_t index, const std::size_t count) noexcept -> Rgb {
    const auto phase   = index * 1536 / count;
    const auto sector  = phase / 256;
    const auto value   = static_cast<std::uint8_t>(phase % 256);
    const auto inverse = static_cast<std::uint8_t>(255U - value);

    switch (sector) {
        case 0:
            return { 255, value, 0 };
        case 1:
            return { inverse, 255, 0 };
        case 2:
            return { 0, 255, value };
        case 3:
            return { 0, inverse, 255 };
        case 4:
            return { value, 0, 255 };
        case 5:
            return { 255, 0, inverse };
        default:
            std::unreachable();
    }
}

template <std::size_t Count> [[nodiscard]] consteval auto make_color_table() -> std::array<Rgb, Count> {
    std::array<Rgb, Count> colors {};
    std::ranges::copy(std::views::iota(std::size_t { 0 }, Count)
            | std::views::transform([](const std::size_t index) { return color_at(index, Count); }),
        colors.begin());
    return colors;
}

inline constexpr auto default_colors = make_color_table<ColorCount::maximum>();

inline constexpr auto printable_glyphs = [] consteval {
    std::array<Glyph, 94> glyphs {};
    for (auto [character, glyph] : std::views::zip(std::views::iota('!'), glyphs)) {
        glyph.bytes.front() = character;
        glyph.length        = 1;
    }
    return glyphs;
}();

[[nodiscard]] inline auto make_colors(const std::size_t count) -> std::vector<Rgb> {
    return std::views::iota(std::size_t { 0 }, count)
        | std::views::transform([count](const std::size_t index) noexcept { return color_at(index, count); })
        | std::ranges::to<std::vector>();
}

template <typename T> [[nodiscard]] auto cycle(const std::span<const T> values) {
    return std::views::repeat(values) | std::views::join;
}

template <std::integral Integer>
[[nodiscard]] auto parse_integer(const std::string_view text, const int base = 10) -> Result<Integer> {
    Integer value {};
    const auto first        = text.data();
    const auto last         = first + text.size();
    const auto [end, error] = std::from_chars(first, last, value, base);

    if (error != std::errc {} || end != last) { return fail(std::format("'{}' is not a valid number", text)); }
    return value;
}

[[nodiscard]] constexpr auto is_drawable(const std::uint32_t codepoint) noexcept -> bool {
    const auto within = [codepoint](const std::uint32_t first, const std::uint32_t last) noexcept {
        return codepoint >= first && codepoint <= last;
    };
    return codepoint <= 0x10FFFFU && !within(0xD800U, 0xDFFFU) && !within(0x00U, 0x1FU) && !within(0x7FU, 0x9FU);
}

[[nodiscard]] inline auto require_drawable(const std::uint32_t codepoint) -> Result<std::uint32_t> {
    if (!is_drawable(codepoint)) { return fail(std::format("U+{:04X} is not a drawable code point", codepoint)); }
    return codepoint;
}

[[nodiscard]] inline auto encode_scalar(const std::uint32_t codepoint) -> Glyph {
    const std::size_t length = [codepoint] {
        if (codepoint < 0x80U) { return 1uz; }
        if (codepoint < 0x800U) { return 2uz; }
        if (codepoint < 0x10000U) { return 3uz; }
        return 4uz;
    }();

    const auto encode_byte = [codepoint, length](const std::size_t position) noexcept {
        const auto payload = codepoint >> (6U * (length - 1 - position));
        if (position != 0) { return static_cast<char>(0x80U | (payload & 0x3FU)); }
        const auto marker = length == 1 ? 0U : (0xFF00U >> length) & 0xFFU;
        return static_cast<char>(marker | payload);
    };

    Glyph glyph { .length = static_cast<std::uint8_t>(length) };
    std::ranges::copy(
        std::views::iota(std::size_t { 0 }, length) | std::views::transform(encode_byte), glyph.bytes.begin());
    return glyph;
}

[[nodiscard]] inline auto encode_utf8(const std::uint32_t codepoint) -> Result<Glyph> {
    return require_drawable(codepoint).transform(encode_scalar);
}

struct ParseState final {
    Options options {};
    bool color_count_seen {};
};

[[nodiscard]] inline auto with_context(const std::string_view context) {
    return [context](Error error) { return Error { std::format("{}: {}", context, error.message) }; };
}

[[nodiscard]] constexpr auto strip_hex_prefix(std::string_view text) noexcept -> std::string_view {
    if (text.starts_with("0x"sv) || text.starts_with("0X"sv)) { text.remove_prefix(2); }
    return text;
}

[[nodiscard]] inline auto parse_glyph(const std::string_view hex) -> Result<Glyph> {
    return parse_integer<std::uint32_t>(strip_hex_prefix(hex), 16)
        .and_then(encode_utf8)
        .transform_error(with_context("-ch"sv));
}

[[nodiscard]] inline auto parse_color_count(const std::string_view text) -> Result<ColorCount> {
    return parse_integer<std::size_t>(text).and_then(ColorCount::make).transform_error(with_context("num_colors"sv));
}

[[nodiscard]] inline auto apply_argument(ParseState state, const std::string_view argument) -> Result<ParseState> {
    if (const auto flag = std::ranges::find(color_flags, argument, &ColorFlag::name); flag != color_flags.end()) {
        state.options.color_mode = flag->mode;
        return state;
    }
    if (argument.starts_with(glyph_prefix)) {
        auto glyph = parse_glyph(argument.substr(glyph_prefix.size()));
        if (!glyph) { return std::unexpected { std::move(glyph).error() }; }
        state.options.glyph = *glyph;
        return state;
    }
    if (argument.starts_with('-')) { return fail(std::format("unknown option: {}", argument)); }
    if (state.color_count_seen) { return fail("only one num_colors argument is allowed"); }
    auto count = parse_color_count(argument);
    if (!count) { return std::unexpected { std::move(count).error() }; }
    state.options.color_count = *count;
    state.color_count_seen    = true;
    return state;
}

[[nodiscard]] inline auto parse_options(const std::span<const std::string_view> arguments) -> Result<Options> {
    ParseState state;
    for (const auto argument : arguments) {
        auto result = apply_argument(std::move(state), argument);
        if (!result) { return std::unexpected { std::move(result).error() }; }
        state = std::move(*result);
    }
    return state.options;
}

inline constexpr std::string_view usage { "Usage: colourbench [options] [num_colors]\n"
                                          "\n"
                                          "Options:\n"
                                          "  -ng                No colors\n"
                                          "  -fg                Foreground colors only\n"
                                          "  -bg                Background colors only\n"
                                          "  -ch=<codepoint>    Draw this specific codepoint (hex) only\n"
                                          "  -h, --help         Show this help message\n"
                                          "\n"
                                          "num_colors is an integer from 1 to 1530 (default: 1530).\n" };

inline void print_usage(std::FILE* output) {
    std::print(output, "{}", usage);
}

inline void append_fixed(std::string& destination, const double value, const int precision) {
    std::array<char, 64> buffer {};
    const auto [end, error]
        = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, precision);
    if (error == std::errc {}) { destination.append(buffer.data(), end); }
}

class RainbowBuffer final {
public:
    explicit RainbowBuffer(const Options& options)
        : color_mode_ { options.color_mode }
        , color_count_ { options.color_count.value() } {
        if (color_count_ == ColorCount::maximum) {
            colors_ = default_colors;
        } else {
            owned_colors_ = make_colors(color_count_);
            colors_       = owned_colors_;
        }

        if (options.glyph) {
            owned_glyph_.fill(*options.glyph);
            glyphs_ = owned_glyph_;
        } else {
            glyphs_ = printable_glyphs;
        }

        rebuild(0);
    }

    RainbowBuffer(const RainbowBuffer&)                    = delete;
    auto operator=(const RainbowBuffer&) -> RainbowBuffer& = delete;
    RainbowBuffer(RainbowBuffer&&)                         = delete;
    auto operator=(RainbowBuffer&&) -> RainbowBuffer&      = delete;
    ~RainbowBuffer()                                       = default;

    [[nodiscard]] auto columns() const noexcept -> std::size_t { return columns_; }

    void rebuild(const std::size_t columns) {
        columns_ = columns;
        text_.clear();
        offsets_.clear();

        const auto entry_count       = color_count_ + columns_;
        const auto foreground_offset = std::max<std::size_t>(1, (color_count_ + 5) / 10);
        offsets_.reserve(entry_count + 1);
        text_.reserve(entry_count * estimated_bytes_per_cell);

        auto cells
            = std::views::zip(cycle(colors_),
                  cycle(colors_) | std::views::drop(static_cast<std::ptrdiff_t>(foreground_offset)), cycle(glyphs_))
            | std::views::take(static_cast<std::ptrdiff_t>(entry_count));

        for (const auto [base, shifted, glyph] : cells) {
            offsets_.push_back(text_.size());
            append_color_escape(base, shifted);
            text_.append(glyph.view());
        }
        offsets_.push_back(text_.size());
    }

    void append_row(std::string& destination, const std::size_t phase, const std::size_t covered_columns = 0) const {
        const auto cell_count = columns_ - std::min(covered_columns, columns_);
        const auto bounds     = std::span<const std::size_t> { offsets_ }.subspan(phase % color_count_, cell_count + 1);
        destination.append(text_, bounds.front(), bounds.back() - bounds.front());
    }

private:
    void append_component(const std::uint8_t value) { text_.append(decimal_byte_table[value].view()); }

    void append_triplet(const Rgb color) {
        append_component(color.red);
        text_.push_back(';');
        append_component(color.green);
        text_.push_back(';');
        append_component(color.blue);
    }

    void append_color_escape(const Rgb base, const Rgb shifted) {
        switch (color_mode_) {
            case ColorMode::all:
                text_.append("\x1b[48;2;"sv);
                append_triplet(base);
                text_.append(";38;2;"sv);
                append_triplet(shifted);
                text_.push_back('m');
                break;
            case ColorMode::foreground:
                text_.append("\x1b[38;2;"sv);
                append_triplet(base);
                text_.push_back('m');
                break;
            case ColorMode::background:
                text_.append("\x1b[48;2;"sv);
                append_triplet(base);
                text_.push_back('m');
                break;
            case ColorMode::none:
                break;
        }
    }

    ColorMode color_mode_;
    std::size_t color_count_;
    std::size_t columns_ {};
    std::vector<Rgb> owned_colors_;
    std::array<Glyph, 1> owned_glyph_ {};
    std::span<const Rgb> colors_;
    std::span<const Glyph> glyphs_;
    std::string text_;
    std::vector<std::size_t> offsets_;
};

class FrameRenderer final {
public:
    explicit FrameRenderer(const Options& options)
        : rainbow_ { options } {}

    void resize(const ScreenSize size) {
        rows_ = size.rows;
        rainbow_.rebuild(size.columns);
        frame_.reserve(size.columns * size.rows * estimated_bytes_per_cell + frame_sequence_margin);
    }

    [[nodiscard]] auto render(const std::size_t animation_frame, const double frames_per_second,
        const double megabytes_per_second) -> std::string_view {
        statistics_.clear();
        append_fixed(statistics_, frames_per_second, 1);
        statistics_.append(" fps | "sv);
        append_fixed(statistics_, megabytes_per_second, 3);
        statistics_.append(" MB/s"sv);
        const auto statistics_length = std::min(statistics_.size(), rainbow_.columns());

        frame_.clear();
        frame_.append(begin_frame);

        for (const auto row : std::views::iota(std::size_t { 0 }, rows_)) {
            const auto covered = row == 0 ? statistics_length : std::size_t { 0 };
            frame_.append(statistics_, 0, covered);
            rainbow_.append_row(frame_, animation_frame + covered + row * 2, covered);
        }

        frame_.append(end_frame);
        return frame_;
    }

private:
    RainbowBuffer rainbow_;
    std::size_t rows_ {};
    std::string statistics_;
    std::string frame_;
};

struct Throughput final {
    double frames_per_second {};
    double megabytes_per_second {};
};

class FrameMeter final {
public:
    void record(const std::size_t bytes) noexcept {
        const auto now = Clock::now();
        if (!previous_time_) {
            previous_time_ = now;
            return;
        }

        const auto elapsed = DoubleSeconds { now - *previous_time_ };
        previous_time_     = now;

        if (elapsed <= DoubleSeconds::zero()) [[unlikely]] { return; }
        if (elapsed >= DoubleSeconds { reset_interval }) {
            current_     = {};
            initialized_ = false;
            return;
        }

        const auto instantaneous = Throughput {
            .frames_per_second = DoubleSeconds { 1 } / elapsed,
            .megabytes_per_second
            = (static_cast<double>(bytes) / static_cast<double>(std::mega::num)) / elapsed.count(),
        };

        const auto alpha = 1.0 - std::exp(-(elapsed / response_time));
        if (!initialized_) {
            current_     = instantaneous;
            initialized_ = true;
            return;
        }

        current_.frames_per_second += alpha * (instantaneous.frames_per_second - current_.frames_per_second);
        current_.megabytes_per_second += alpha * (instantaneous.megabytes_per_second - current_.megabytes_per_second);
    }

    [[nodiscard]] auto current() const noexcept -> Throughput { return current_; }

private:
    using DoubleSeconds                  = std::chrono::duration<double>;
    static constexpr auto response_time  = DoubleSeconds { std::chrono::milliseconds { 500 } };
    static constexpr auto reset_interval = std::chrono::seconds { 2 };
    std::optional<Clock::time_point> previous_time_;
    Throughput current_ {};
    bool initialized_ {};
};

template <typename Backend>
concept TerminalBackend = requires(Backend& terminal, const Backend& const_terminal, std::string_view text) {
    { const_terminal.size() } -> std::same_as<Result<ScreenSize>>;
    { terminal.poll_resize() } -> std::same_as<Result<bool>>;
    { terminal.write(text) } noexcept -> std::same_as<bool>;
    { const_terminal.last_error(text) } -> std::same_as<std::string>;
    { const_terminal.interrupted() } noexcept -> std::same_as<bool>;
};

template <TerminalBackend Backend>
[[nodiscard]] auto refresh_screen(Backend& terminal, FrameRenderer& renderer, ScreenSize& screen) -> Result<bool> {
    const auto resized = terminal.poll_resize();
    if (!resized) { return std::unexpected { resized.error() }; }
    if (!*resized && screen.valid()) { return true; }

    const auto size = terminal.size();
    if (!size) { return std::unexpected { size.error() }; }

    screen = *size;
    if (!screen.valid()) {
        std::this_thread::sleep_for(idle_poll_interval);
        return false;
    }

    renderer.resize(screen);
    return true;
}

template <TerminalBackend Backend> [[nodiscard]] auto run(Backend& terminal, const Options& options) -> Result<void> {
    if (!terminal.write(begin_screen)) { return fail(terminal.last_error("write"sv)); }
    const scope_exit leave_screen { [&terminal]() noexcept { static_cast<void>(terminal.write(end_screen)); } };

    FrameRenderer renderer { options };
    FrameMeter meter;
    ScreenSize screen {};
    std::size_t animation_frame {};

    while (!terminal.interrupted()) {
        const auto ready = refresh_screen(terminal, renderer, screen);
        if (!ready) { return std::unexpected { ready.error() }; }
        if (!*ready) { continue; }

        const auto [frames_per_second, megabytes_per_second] = meter.current();
        const auto frame = renderer.render(animation_frame, frames_per_second, megabytes_per_second);
        if (!terminal.write(frame)) { return fail(terminal.last_error("write"sv)); }

        meter.record(frame.size());
        ++animation_frame;
    }

    return {};
}

} // namespace colourbench

namespace colourbench::platform {

inline std::atomic<bool> interrupt_flag { false };

[[nodiscard]] inline auto win32_message(const std::string_view action) -> std::string {
    const std::error_code code { static_cast<std::int32_t>(::GetLastError()), std::system_category() };
    return std::format("{} failed: {}", action, code.message());
}

inline auto WINAPI on_control_event(const DWORD event) noexcept -> BOOL {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) { return FALSE; }
    interrupt_flag.store(true, std::memory_order_relaxed);
    return TRUE;
}

[[nodiscard]] inline auto standard_handle(const DWORD id, const std::string_view name) -> Result<HANDLE> {
    const auto handle = ::GetStdHandle(id);
    if (handle == INVALID_HANDLE_VALUE) { return fail(win32_message(std::format("GetStdHandle ({})", name))); }
    if (handle == nullptr) { return fail(std::format("no {} handle is attached", name)); }
    return handle;
}

[[nodiscard]] inline auto console_mode(const HANDLE console) -> Result<DWORD> {
    DWORD mode {};
    if (!::GetConsoleMode(console, &mode)) {
        return fail(win32_message("GetConsoleMode (stdin and stdout must be a console)"sv));
    }
    return mode;
}

[[nodiscard]] inline auto set_console_mode(const HANDLE console, const DWORD mode) -> Result<void> {
    if (!::SetConsoleMode(console, mode)) { return fail(win32_message("SetConsoleMode"sv)); }
    return {};
}

[[nodiscard]] inline auto enable_console_flags(const HANDLE console, const DWORD flags) -> Result<DWORD> {
    return console_mode(console).and_then([console, flags](const DWORD previous) {
        return set_console_mode(console, previous | flags).transform([previous] { return previous; });
    });
}

class Console final {
public:
    Console(const HANDLE input, const HANDLE output) noexcept
        : input_ { input }
        , output_ { output } {}

    [[nodiscard]] auto size() const -> Result<ScreenSize> {
        CONSOLE_SCREEN_BUFFER_INFO information {};
        if (!::GetConsoleScreenBufferInfo(output_, &information)) {
            return fail(win32_message("GetConsoleScreenBufferInfo"sv));
        }

        const auto extent = [](const SHORT first, const SHORT last) noexcept {
            return static_cast<std::size_t>(std::max(last - first + 1, 0));
        };
        return ScreenSize {
            extent(information.srWindow.Left, information.srWindow.Right),
            extent(information.srWindow.Top, information.srWindow.Bottom),
        };
    }

    [[nodiscard]] auto poll_resize() -> Result<bool> {
        DWORD remaining {};
        if (!::GetNumberOfConsoleInputEvents(input_, &remaining)) {
            return fail(win32_message("GetNumberOfConsoleInputEvents"sv));
        }
        if (remaining == 0) { return false; }

        std::array<INPUT_RECORD, 128> records {};
        bool resized {};
        while (remaining != 0) {
            const auto batch = static_cast<DWORD>(std::min<std::size_t>(remaining, records.size()));
            DWORD read {};
            if (!::ReadConsoleInputW(input_, records.data(), batch, &read)) {
                return fail(win32_message("ReadConsoleInputW"sv));
            }
            if (read == 0) { break; }

            resized = resized
                || std::ranges::any_of(records | std::views::take(read),
                    [](const INPUT_RECORD& record) noexcept { return record.EventType == WINDOW_BUFFER_SIZE_EVENT; });
            remaining -= read;
        }
        return resized;
    }

    [[nodiscard]] auto write(std::string_view text) noexcept -> bool {
        while (!text.empty()) {
            const auto chunk
                = static_cast<DWORD>(std::min<std::size_t>(text.size(), std::numeric_limits<DWORD>::max()));
            DWORD written {};
            if (!::WriteFile(output_, text.data(), chunk, &written, nullptr)) { return false; }
            if (written == 0) {
                ::SetLastError(ERROR_WRITE_FAULT);
                return false;
            }
            text.remove_prefix(written);
        }
        return true;
    }

    [[nodiscard]] auto last_error(const std::string_view action) const -> std::string { return win32_message(action); }

    [[nodiscard]] auto interrupted() const noexcept -> bool { return interrupt_flag.load(std::memory_order_relaxed); }

    [[nodiscard]] auto input() const noexcept -> HANDLE { return input_; }

    [[nodiscard]] auto output() const noexcept -> HANDLE { return output_; }

private:
    HANDLE input_;
    HANDLE output_;
};

static_assert(TerminalBackend<Console>);

[[nodiscard]] inline auto open_console() -> Result<Console> {
    return standard_handle(STD_INPUT_HANDLE, "stdin"sv).and_then([](const HANDLE input) {
        return standard_handle(STD_OUTPUT_HANDLE, "stdout"sv).transform([input](const HANDLE output) {
            return Console { input, output };
        });
    });
}

[[nodiscard]] inline auto run_on_console(const Options& options) -> Result<void> {
    auto opened = open_console();
    if (!opened) { return std::unexpected { std::move(opened).error() }; }
    Console console     = std::move(*opened);
    const HANDLE input  = console.input();
    const HANDLE output = console.output();

    interrupt_flag.store(false, std::memory_order_relaxed);
    if (!::SetConsoleCtrlHandler(&on_control_event, TRUE)) { return fail(win32_message("SetConsoleCtrlHandler"sv)); }
    const scope_exit remove_control_handler { []() noexcept { ::SetConsoleCtrlHandler(&on_control_event, FALSE); } };

    const auto previous_code_page = ::GetConsoleOutputCP();
    if (previous_code_page == 0) { return fail(win32_message("GetConsoleOutputCP"sv)); }
    if (!::SetConsoleOutputCP(CP_UTF8)) { return fail(win32_message("SetConsoleOutputCP"sv)); }
    const scope_exit restore_code_page { [previous_code_page]() noexcept {
        ::SetConsoleOutputCP(previous_code_page);
    } };

    const auto previous_output_mode
        = enable_console_flags(output, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    if (!previous_output_mode) { return std::unexpected { previous_output_mode.error() }; }
    const scope_exit restore_output_mode { [output, mode = *previous_output_mode]() noexcept {
        ::SetConsoleMode(output, mode);
    } };

    const auto previous_input_mode = enable_console_flags(input, ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT);
    if (!previous_input_mode) { return std::unexpected { previous_input_mode.error() }; }
    const scope_exit restore_input_mode { [input, mode = *previous_input_mode]() noexcept {
        ::SetConsoleMode(input, mode);
    } };

    return run(console, options);
}

} // namespace colourbench::platform

int main(int argc, char* argv[]) {
    using namespace std::literals;

    const auto arguments = std::span { argv, static_cast<std::size_t>(argc) } | std::views::drop(1)
        | std::ranges::to<std::vector<std::string_view>>();

    if (std::ranges::contains(arguments, "-h"sv) || std::ranges::contains(arguments, "--help"sv)) {
        colourbench::print_usage(stdout);
        return EXIT_SUCCESS;
    }

    const auto options = colourbench::parse_options(arguments);
    if (!options) {
        std::print(stderr, "Error: {}\n\n", options.error().message);
        colourbench::print_usage(stderr);
        return EXIT_FAILURE;
    }

    if (const auto result = colourbench::platform::run_on_console(*options); !result) {
        std::print(stderr, "colourbench: {}\n", result.error().message);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
