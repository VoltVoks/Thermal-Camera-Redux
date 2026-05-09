#include <opencv2/opencv.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kSensorWidth = 256;
constexpr int kSensorHeight = 192;
constexpr int kFrameRowsWithMeta = 196;
constexpr int kDefaultRoi = 5;
constexpr double kDefaultRawSlope = 1.0 / 160.0;
constexpr double kMinRawSpanForSlopeFit = 300.0;
constexpr double kMinReferenceSpanForSlopeFit = 5.0;
constexpr double kMinPlausibleSlope = 1.0 / 320.0;
constexpr double kMaxPlausibleSlope = 1.0 / 80.0;
constexpr const char *kDefaultSerial = "EA5243009";
constexpr const char *kDefaultById =
    "/dev/v4l/by-id/usb-Camera_USB_Camera_EA5243009-video-index0";

struct Palette {
    const char *name;
    int id;
};

const std::vector<Palette> kPalettes = {
    {"Gray", -1},
    {"Inferno", cv::COLORMAP_INFERNO},
    {"Magma", cv::COLORMAP_MAGMA},
    {"Turbo", cv::COLORMAP_TURBO},
    {"Viridis", cv::COLORMAP_VIRIDIS},
    {"Cividis", cv::COLORMAP_CIVIDIS},
    {"Jet", cv::COLORMAP_JET},
    {"Bone", cv::COLORMAP_BONE},
    {"Parula", cv::COLORMAP_PARULA},
};

enum class RangeMode {
    CalibratedRawC,
    MetadataDeciF,
    MetadataDeciC,
    RawCounts,
};

struct ReferencePoint {
    double raw = 0.0;
    double referenceCelsius = 0.0;
};

struct Calibration {
    std::string serial = kDefaultSerial;
    std::string path;
    double slope = kDefaultRawSlope;
    double offsetCelsius = 0.0;
    std::vector<ReferencePoint> references;
    bool loaded = false;
};

struct ViewerState {
    int scale = 4;
    int paletteIndex = 1;
    bool showHud = true;
    bool displayCelsius = true;
    bool fullscreen = false;
    bool freeze = false;
    bool debugMeta = false;
    bool flipHorizontal = false;
    bool flipVertical = false;
    int rotationQuarterTurns = 1;
    RangeMode rangeMode = RangeMode::CalibratedRawC;
    cv::Point cursor{kSensorWidth / 2, kSensorHeight / 2};
};

struct Options {
    std::string device;
    std::string profile = "auto";
    std::string calibrationFile;
    std::optional<double> captureReferenceCelsius;
    int referenceRoi = kDefaultRoi;
    int scale = 4;
    bool debugMeta = false;
    bool help = false;
};

struct DecodedFrame {
    cv::Mat raw16;
    std::array<uint16_t, 32> meta{};
    bool metaValid = false;
    int chosenStartRow = 0;
};

bool startsWith(const std::string &value, const std::string &prefix) {
    return value.rfind(prefix, 0) == 0;
}

uint16_t readLE16(const uint8_t *ptr) {
    return static_cast<uint16_t>(ptr[0]) | (static_cast<uint16_t>(ptr[1]) << 8);
}

double fToC(double f) {
    return (f - 32.0) * 5.0 / 9.0;
}

double cToF(double c) {
    return c * 9.0 / 5.0 + 32.0;
}

std::string formatDouble(double value, int decimals = 1) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    return oss.str();
}

std::string rangeModeLabel(RangeMode mode) {
    switch (mode) {
        case RangeMode::CalibratedRawC:
            return "raw affine C";
        case RangeMode::MetadataDeciF:
            return "meta/10 F";
        case RangeMode::MetadataDeciC:
            return "meta/10 C";
        case RangeMode::RawCounts:
            return "raw counts";
    }
    return "unknown";
}

std::string unitLabel(bool celsius) {
    return celsius ? "C" : "F";
}

std::string fourccString(double value) {
    const int fourcc = static_cast<int>(value);
    std::string out;
    out.push_back(static_cast<char>(fourcc & 0xFF));
    out.push_back(static_cast<char>((fourcc >> 8) & 0xFF));
    out.push_back(static_cast<char>((fourcc >> 16) & 0xFF));
    out.push_back(static_cast<char>((fourcc >> 24) & 0xFF));
    return out;
}

std::string nowIsoLike() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    return buf;
}

std::string defaultDevicePath() {
    if (std::filesystem::exists(kDefaultById)) {
        return kDefaultById;
    }
    return "/dev/video2";
}

std::string serialFromDevice(const std::string &device) {
    std::filesystem::path path(device);
    const std::string name = path.filename().string();
    const std::string marker = "_USB_Camera_";
    const size_t startMarker = name.find(marker);
    if (startMarker == std::string::npos) {
        return kDefaultSerial;
    }
    const size_t start = startMarker + marker.size();
    const size_t end = name.find("-video", start);
    if (end == std::string::npos || end <= start) {
        return kDefaultSerial;
    }
    return name.substr(start, end - start);
}

std::string defaultCalibrationPath(const std::string &serial) {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path root;
    if (xdg && *xdg) {
        root = xdg;
    } else {
        const char *home = std::getenv("HOME");
        root = home && *home ? std::filesystem::path(home) / ".config" : std::filesystem::path(".config");
    }
    return (root / "thermal-camera-redux" / (serial + "-calibration.json")).string();
}

std::optional<double> extractJsonNumber(const std::string &text, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const char *start = text.c_str() + pos + 1;
    char *end = nullptr;
    const double value = std::strtod(start, &end);
    if (end == start) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> extractJsonString(const std::string &text, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const size_t end = text.find('"', pos + 1);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return text.substr(pos + 1, end - pos - 1);
}

void fitCalibration(Calibration &calibration) {
    if (calibration.references.empty()) {
        calibration.slope = kDefaultRawSlope;
        calibration.offsetCelsius = 0.0;
        return;
    }

    if (calibration.references.size() == 1) {
        calibration.slope = kDefaultRawSlope;
        calibration.offsetCelsius =
            calibration.references.front().referenceCelsius -
            (calibration.references.front().raw * calibration.slope);
        return;
    }

    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    double minRaw = calibration.references.front().raw;
    double maxRaw = calibration.references.front().raw;
    double minReference = calibration.references.front().referenceCelsius;
    double maxReference = calibration.references.front().referenceCelsius;
    const double n = static_cast<double>(calibration.references.size());
    for (const auto &ref : calibration.references) {
        sx += ref.raw;
        sy += ref.referenceCelsius;
        sxx += ref.raw * ref.raw;
        sxy += ref.raw * ref.referenceCelsius;
        minRaw = std::min(minRaw, ref.raw);
        maxRaw = std::max(maxRaw, ref.raw);
        minReference = std::min(minReference, ref.referenceCelsius);
        maxReference = std::max(maxReference, ref.referenceCelsius);
    }

    if ((maxRaw - minRaw) < kMinRawSpanForSlopeFit ||
        (maxReference - minReference) < kMinReferenceSpanForSlopeFit) {
        calibration.slope = kDefaultRawSlope;
        calibration.offsetCelsius = 0.0;
        for (const auto &ref : calibration.references) {
            calibration.offsetCelsius += ref.referenceCelsius - (ref.raw * calibration.slope);
        }
        calibration.offsetCelsius /= n;
        return;
    }

    const double denom = (n * sxx) - (sx * sx);
    if (std::abs(denom) < 1e-9) {
        calibration.slope = kDefaultRawSlope;
        calibration.offsetCelsius = (sy / n) - ((sx / n) * calibration.slope);
        return;
    }

    const double fittedSlope = ((n * sxy) - (sx * sy)) / denom;
    if (fittedSlope < kMinPlausibleSlope || fittedSlope > kMaxPlausibleSlope) {
        calibration.slope = kDefaultRawSlope;
        calibration.offsetCelsius = 0.0;
        for (const auto &ref : calibration.references) {
            calibration.offsetCelsius += ref.referenceCelsius - (ref.raw * calibration.slope);
        }
        calibration.offsetCelsius /= n;
        return;
    }

    calibration.slope = fittedSlope;
    calibration.offsetCelsius = (sy - (calibration.slope * sx)) / n;
}

Calibration loadCalibration(const std::string &path, const std::string &serial) {
    Calibration calibration;
    calibration.serial = serial;
    calibration.path = path;

    std::ifstream in(path);
    if (!in) {
        return calibration;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    if (auto parsed = extractJsonString(text, "serial")) {
        calibration.serial = *parsed;
    }
    if (auto parsed = extractJsonNumber(text, "slope")) {
        calibration.slope = *parsed;
    }
    if (auto parsed = extractJsonNumber(text, "offset_celsius")) {
        calibration.offsetCelsius = *parsed;
    }

    size_t pos = 0;
    while ((pos = text.find("\"raw\"", pos)) != std::string::npos) {
        const size_t objectEnd = text.find('}', pos);
        if (objectEnd == std::string::npos) {
            break;
        }
        const std::string object = text.substr(pos, objectEnd - pos + 1);
        const auto raw = extractJsonNumber(object, "raw");
        const auto reference = extractJsonNumber(object, "reference_celsius");
        if (raw && reference) {
            calibration.references.push_back({*raw, *reference});
        }
        pos = objectEnd + 1;
    }

    calibration.loaded = true;
    if (!calibration.references.empty()) {
        fitCalibration(calibration);
    }
    return calibration;
}

bool saveCalibration(const Calibration &calibration, std::string *error) {
    const std::filesystem::path path(calibration.path);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error) {
                *error = "create_directories failed: " + ec.message();
            }
            return false;
        }
    }

    std::ofstream out(calibration.path, std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "failed to open " + calibration.path;
        }
        return false;
    }

    out << "{\n";
    out << "  \"schema\": 1,\n";
    out << "  \"serial\": \"" << calibration.serial << "\",\n";
    out << "  \"model\": \"raw_affine_celsius\",\n";
    out << "  \"slope\": " << std::setprecision(12) << calibration.slope << ",\n";
    out << "  \"offset_celsius\": " << std::setprecision(12) << calibration.offsetCelsius << ",\n";
    out << "  \"updated_at\": \"" << nowIsoLike() << "\",\n";
    out << "  \"references\": [\n";
    for (size_t i = 0; i < calibration.references.size(); ++i) {
        const auto &ref = calibration.references[i];
        out << "    {\"raw\": " << std::setprecision(12) << ref.raw
            << ", \"reference_celsius\": " << std::setprecision(12) << ref.referenceCelsius << "}";
        if (i + 1 != calibration.references.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return true;
}

std::string orientationLabel(const ViewerState &state) {
    std::ostringstream oss;
    oss << "rot=" << ((state.rotationQuarterTurns % 4 + 4) % 4) * 90 << "cw";
    if (state.flipHorizontal) {
        oss << " flipH";
    }
    if (state.flipVertical) {
        oss << " flipV";
    }
    return oss.str();
}

cv::Size displayFrameSize(const ViewerState &state) {
    const int turns = (state.rotationQuarterTurns % 4 + 4) % 4;
    if (turns % 2 == 1) {
        return cv::Size(kSensorHeight, kSensorWidth);
    }
    return cv::Size(kSensorWidth, kSensorHeight);
}

void updateWindowSize(const std::string &windowName, const ViewerState &state) {
    const cv::Size oriented = displayFrameSize(state);
    cv::resizeWindow(windowName, oriented.width * state.scale, oriented.height * state.scale);
}

cv::Point sensorToDisplayPoint(const cv::Point &sensor, const ViewerState &state) {
    const int turns = (state.rotationQuarterTurns % 4 + 4) % 4;
    int x = sensor.x;
    int y = sensor.y;
    int width = kSensorWidth;
    int height = kSensorHeight;

    switch (turns) {
        case 0:
            break;
        case 1: {
            const int oldY = y;
            y = x;
            x = height - 1 - oldY;
            std::swap(width, height);
            break;
        }
        case 2:
            x = width - 1 - x;
            y = height - 1 - y;
            break;
        case 3: {
            const int oldX = x;
            x = y;
            y = width - 1 - oldX;
            std::swap(width, height);
            break;
        }
    }

    if (state.flipHorizontal) {
        x = width - 1 - x;
    }
    if (state.flipVertical) {
        y = height - 1 - y;
    }
    return cv::Point(x, y);
}

cv::Point displayToSensorPoint(const cv::Point &display, const ViewerState &state) {
    const int turns = (state.rotationQuarterTurns % 4 + 4) % 4;
    cv::Size oriented = displayFrameSize(state);

    int x = std::clamp(display.x, 0, oriented.width - 1);
    int y = std::clamp(display.y, 0, oriented.height - 1);

    if (state.flipHorizontal) {
        x = oriented.width - 1 - x;
    }
    if (state.flipVertical) {
        y = oriented.height - 1 - y;
    }

    switch (turns) {
        case 0:
            return cv::Point(x, y);
        case 1:
            return cv::Point(y, kSensorHeight - 1 - x);
        case 2:
            return cv::Point(kSensorWidth - 1 - x, kSensorHeight - 1 - y);
        case 3:
            return cv::Point(kSensorWidth - 1 - y, x);
    }

    return cv::Point(x, y);
}

bool metadataMarkerMatches(const uint8_t *row193) {
    return readLE16(row193) == 0xccdd && readLE16(row193 + 2) == 0xaabb;
}

void addCandidate(std::vector<int> *candidates, int start) {
    if (start < 0) {
        return;
    }
    if (std::find(candidates->begin(), candidates->end(), start) == candidates->end()) {
        candidates->push_back(start);
    }
}

DecodedFrame decodeFrame(const cv::Mat &frame) {
    DecodedFrame decoded;
    decoded.raw16 = cv::Mat(kSensorHeight, kSensorWidth, CV_16UC1);

    std::vector<int> candidates;
    addCandidate(&candidates, 0);
    if (frame.rows >= 2 * kFrameRowsWithMeta) {
        addCandidate(&candidates, kFrameRowsWithMeta);
    }
    if (frame.rows >= kFrameRowsWithMeta) {
        addCandidate(&candidates, frame.rows - kFrameRowsWithMeta);
    }

    int bestStart = 0;
    bool foundMarker = false;
    for (int start : candidates) {
        if (start + kFrameRowsWithMeta > frame.rows) {
            continue;
        }
        if (metadataMarkerMatches(frame.ptr<uint8_t>(start + 193))) {
            bestStart = start;
            foundMarker = true;
            break;
        }
    }
    decoded.chosenStartRow = bestStart;

    for (int row = 0; row < kSensorHeight; ++row) {
        std::memcpy(
            decoded.raw16.ptr<uint16_t>(row),
            frame.ptr<uint8_t>(bestStart + row),
            static_cast<size_t>(kSensorWidth) * sizeof(uint16_t));
    }

    if (bestStart + kFrameRowsWithMeta <= frame.rows) {
        const uint8_t *meta0 = frame.ptr<uint8_t>(bestStart + 192);
        const uint8_t *meta1 = frame.ptr<uint8_t>(bestStart + 193);
        for (int i = 0; i < 16; ++i) {
            decoded.meta[i] = readLE16(meta0 + i * 2);
            decoded.meta[16 + i] = readLE16(meta1 + i * 2);
        }
        decoded.metaValid = foundMarker;
    }

    return decoded;
}

bool isSupportedFrame(const cv::Mat &frame, std::string *error) {
    if (frame.empty()) {
        if (error) {
            *error = "empty frame";
        }
        return false;
    }
    if (frame.type() != CV_8UC2 || frame.cols != kSensorWidth || frame.rows < kSensorHeight) {
        if (error) {
            std::ostringstream oss;
            oss << "unexpected frame type/size: type=" << frame.type()
                << " cols=" << frame.cols << " rows=" << frame.rows;
            *error = oss.str();
        }
        return false;
    }
    return true;
}

cv::Mat normalizeRaw(const cv::Mat &raw16, double *outMin, double *outMax) {
    double rawMin = 0.0;
    double rawMax = 0.0;
    cv::minMaxLoc(raw16, &rawMin, &rawMax);
    if (outMin) {
        *outMin = rawMin;
    }
    if (outMax) {
        *outMax = rawMax;
    }

    cv::Mat normalized(raw16.size(), CV_8UC1, cv::Scalar(0));
    if (rawMax <= rawMin) {
        return normalized;
    }

    const double scale = 255.0 / (rawMax - rawMin);
    raw16.convertTo(normalized, CV_8UC1, scale, -rawMin * scale);
    return normalized;
}

cv::Mat colorize(const cv::Mat &normalized8, int paletteIndex) {
    cv::Mat colored;
    if (kPalettes[paletteIndex].id < 0) {
        cv::cvtColor(normalized8, colored, cv::COLOR_GRAY2BGR);
    } else {
        cv::applyColorMap(normalized8, colored, kPalettes[paletteIndex].id);
    }
    return colored;
}

cv::Mat applyOrientation(const cv::Mat &image, const ViewerState &state) {
    cv::Mat oriented = image.clone();
    const int turns = (state.rotationQuarterTurns % 4 + 4) % 4;
    switch (turns) {
        case 0:
            break;
        case 1:
            cv::rotate(oriented, oriented, cv::ROTATE_90_CLOCKWISE);
            break;
        case 2:
            cv::rotate(oriented, oriented, cv::ROTATE_180);
            break;
        case 3:
            cv::rotate(oriented, oriented, cv::ROTATE_90_COUNTERCLOCKWISE);
            break;
    }

    if (state.flipHorizontal && state.flipVertical) {
        cv::flip(oriented, oriented, -1);
    } else if (state.flipHorizontal) {
        cv::flip(oriented, oriented, 1);
    } else if (state.flipVertical) {
        cv::flip(oriented, oriented, 0);
    }
    return oriented;
}

double calibratedRawCelsius(const Calibration &calibration, double rawValue) {
    return (rawValue * calibration.slope) + calibration.offsetCelsius;
}

bool mapTemperatureEstimate(
    const ViewerState &state,
    const Calibration &calibration,
    const DecodedFrame &decoded,
    double rawValue,
    double rawMin,
    double rawMax,
    double *displayValue,
    std::string *debugText
) {
    if (state.rangeMode == RangeMode::CalibratedRawC) {
        const double tempC = calibratedRawCelsius(calibration, rawValue);
        if (displayValue) {
            *displayValue = state.displayCelsius ? tempC : cToF(tempC);
        }
        if (debugText) {
            *debugText = rangeModeLabel(state.rangeMode);
        }
        return true;
    }

    if (state.rangeMode == RangeMode::RawCounts) {
        if (displayValue) {
            *displayValue = rawValue;
        }
        if (debugText) {
            *debugText = "raw";
        }
        return true;
    }

    if (!decoded.metaValid || rawMax <= rawMin) {
        return false;
    }

    const double metaMin = decoded.meta[0] / 10.0;
    const double metaMax = decoded.meta[1] / 10.0;
    if (metaMax <= metaMin) {
        return false;
    }

    const double nativeTemp = metaMin + (rawValue - rawMin) * (metaMax - metaMin) / (rawMax - rawMin);
    const double tempC = state.rangeMode == RangeMode::MetadataDeciF ? fToC(nativeTemp) : nativeTemp;

    if (displayValue) {
        *displayValue = state.displayCelsius ? tempC : cToF(tempC);
    }
    if (debugText) {
        *debugText = rangeModeLabel(state.rangeMode);
    }
    return true;
}

double rawRoiAverage(const cv::Mat &raw16, cv::Point center, int roiSize) {
    roiSize = std::max(1, roiSize);
    const int radius = roiSize / 2;
    const int x0 = std::max(0, center.x - radius);
    const int y0 = std::max(0, center.y - radius);
    const int x1 = std::min(raw16.cols - 1, center.x + radius);
    const int y1 = std::min(raw16.rows - 1, center.y + radius);

    double total = 0.0;
    int count = 0;
    for (int y = y0; y <= y1; ++y) {
        const auto *row = raw16.ptr<uint16_t>(y);
        for (int x = x0; x <= x1; ++x) {
            total += row[x];
            count++;
        }
    }
    return count > 0 ? total / count : 0.0;
}

void drawTextBlock(
    cv::Mat &image,
    const std::vector<std::string> &lines,
    const cv::Point &origin
) {
    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double scale = 0.55;
    const int thickness = 1;
    const int padding = 8;
    const int lineHeight = 22;
    int width = 0;
    for (const auto &line : lines) {
        int baseline = 0;
        const auto size = cv::getTextSize(line, font, scale, thickness, &baseline);
        width = std::max(width, size.width);
    }

    cv::Rect box(origin.x, origin.y, width + 2 * padding, static_cast<int>(lines.size()) * lineHeight + 2 * padding);
    box &= cv::Rect(0, 0, image.cols, image.rows);
    cv::rectangle(image, box, cv::Scalar(20, 20, 20), cv::FILLED);
    cv::rectangle(image, box, cv::Scalar(70, 70, 70), 1);

    for (size_t i = 0; i < lines.size(); ++i) {
        cv::putText(
            image,
            lines[i],
            cv::Point(box.x + padding, box.y + padding + static_cast<int>(i + 1) * lineHeight - 6),
            font,
            scale,
            cv::Scalar(235, 235, 235),
            thickness,
            cv::LINE_AA);
    }
}

void drawCrosshair(cv::Mat &image, const ViewerState &state) {
    const cv::Point oriented = sensorToDisplayPoint(state.cursor, state);
    const int sx = oriented.x * state.scale + state.scale / 2;
    const int sy = oriented.y * state.scale + state.scale / 2;
    const int arm = std::max(8, state.scale * 3);
    const cv::Scalar color(255, 255, 255);
    const cv::Scalar outline(0, 0, 0);

    cv::line(image, cv::Point(sx - arm, sy), cv::Point(sx + arm, sy), outline, 3, cv::LINE_AA);
    cv::line(image, cv::Point(sx, sy - arm), cv::Point(sx, sy + arm), outline, 3, cv::LINE_AA);
    cv::line(image, cv::Point(sx - arm, sy), cv::Point(sx + arm, sy), color, 1, cv::LINE_AA);
    cv::line(image, cv::Point(sx, sy - arm), cv::Point(sx, sy + arm), color, 1, cv::LINE_AA);
}

void onMouse(int event, int x, int y, int, void *userdata) {
    auto *state = static_cast<ViewerState *>(userdata);
    if (event != cv::EVENT_MOUSEMOVE && event != cv::EVENT_LBUTTONDOWN) {
        return;
    }
    const cv::Point displayPoint(
        x / std::max(1, state->scale),
        y / std::max(1, state->scale));
    state->cursor = displayToSensorPoint(displayPoint, *state);
}

void printHelp() {
    std::cout
        << "Usage: mini640-viewer [options] [device]\n\n"
        << "Options:\n"
        << "  --camera-profile auto|mini640   Accept Mini640 profile selection\n"
        << "  --device <path>                 Video node or /dev/v4l/by-id path\n"
        << "  --calibration-file <path>       Override calibration JSON path\n"
        << "  --capture-reference <celsius>   Capture center ROI and add thermometer reference\n"
        << "  --reference-roi <pixels>        ROI size for reference capture (default 5)\n"
        << "  --debug-meta                    Start with metadata diagnostics visible\n"
        << "  --scale <n>                     Initial integer scale, 1..8\n"
        << "  -h, --help                      Show this help\n\n"
        << "Controls:\n"
        << "  q: quit\n"
        << "  j/m: next/previous palette\n"
        << "  t: toggle C/F display\n"
        << "  r: cycle temp source (raw affine C, meta/10 F, meta/10 C, raw counts)\n"
        << "  8/9: rotate clockwise / counterclockwise\n"
        << "  x/y: flip horizontal / vertical\n"
        << "  -/=: adjust affine offset by 0.5 C and save calibration\n"
        << "  0: refit calibration from saved references, or reset to raw/160\n"
        << "  [ / ]: zoom out / in\n"
        << "  h: toggle HUD\n"
        << "  d: toggle metadata debug lines\n"
        << "  space: freeze/unfreeze current frame\n"
        << "  f: toggle fullscreen\n";
}

bool parseDoubleArg(const std::string &text, double *out) {
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

bool parseIntArg(const std::string &text, int *out) {
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool parseArgs(int argc, char **argv, Options *options) {
    options->device = defaultDevicePath();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char *name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << name << " needs a value\n";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "-h" || arg == "--help") {
            options->help = true;
            return true;
        }
        if (arg == "--camera-profile" || arg == "--profile") {
            auto value = needValue(arg.c_str());
            if (!value) {
                return false;
            }
            options->profile = *value;
        } else if (startsWith(arg, "--camera-profile=")) {
            options->profile = arg.substr(std::strlen("--camera-profile="));
        } else if (arg == "--device" || arg == "-d") {
            auto value = needValue(arg.c_str());
            if (!value) {
                return false;
            }
            options->device = *value;
        } else if (startsWith(arg, "--device=")) {
            options->device = arg.substr(std::strlen("--device="));
        } else if (arg == "--calibration-file") {
            auto value = needValue(arg.c_str());
            if (!value) {
                return false;
            }
            options->calibrationFile = *value;
        } else if (startsWith(arg, "--calibration-file=")) {
            options->calibrationFile = arg.substr(std::strlen("--calibration-file="));
        } else if (arg == "--capture-reference") {
            auto value = needValue(arg.c_str());
            double parsed = 0.0;
            if (!value || !parseDoubleArg(*value, &parsed)) {
                std::cerr << "--capture-reference needs a Celsius number\n";
                return false;
            }
            options->captureReferenceCelsius = parsed;
        } else if (startsWith(arg, "--capture-reference=")) {
            double parsed = 0.0;
            if (!parseDoubleArg(arg.substr(std::strlen("--capture-reference=")), &parsed)) {
                std::cerr << "--capture-reference needs a Celsius number\n";
                return false;
            }
            options->captureReferenceCelsius = parsed;
        } else if (arg == "--reference-roi") {
            auto value = needValue(arg.c_str());
            int parsed = 0;
            if (!value || !parseIntArg(*value, &parsed)) {
                std::cerr << "--reference-roi needs an integer\n";
                return false;
            }
            options->referenceRoi = std::clamp(parsed, 1, 51);
        } else if (arg == "--debug-meta") {
            options->debugMeta = true;
        } else if (arg == "--scale") {
            auto value = needValue(arg.c_str());
            int parsed = 0;
            if (!value || !parseIntArg(*value, &parsed)) {
                std::cerr << "--scale needs an integer\n";
                return false;
            }
            options->scale = std::clamp(parsed, 1, 8);
        } else if (!startsWith(arg, "-")) {
            options->device = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    if (options->profile != "auto" && options->profile != "mini640") {
        std::cerr << "mini640-viewer only handles --camera-profile auto|mini640; "
                  << "use run-thermal-camera.sh --camera-profile tc001 for TC001 Redux.\n";
        return false;
    }

    return true;
}

bool configureCapture(cv::VideoCapture *cap, const std::string &device) {
    *cap = cv::VideoCapture(device, cv::CAP_V4L2);
    if (!cap->isOpened()) {
        std::cerr << "Failed to open " << device << "\n";
        return false;
    }

    cap->set(cv::CAP_PROP_CONVERT_RGB, 0.0);
    cap->set(cv::CAP_PROP_MONOCHROME, 1.0);
    cap->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V'));
    cap->set(cv::CAP_PROP_FRAME_WIDTH, kSensorWidth);
    cap->set(cv::CAP_PROP_FRAME_HEIGHT, kFrameRowsWithMeta);
    cap->set(cv::CAP_PROP_FPS, 25.0);
    return true;
}

bool readDecodedFrame(cv::VideoCapture *cap, DecodedFrame *decoded, cv::Mat *lastFrame, std::string *error) {
    cv::Mat frame;
    if (!cap->read(frame) || frame.empty()) {
        if (error) {
            *error = "frame read failed";
        }
        return false;
    }
    if (!isSupportedFrame(frame, error)) {
        return false;
    }
    if (lastFrame) {
        *lastFrame = frame.clone();
    }
    *decoded = decodeFrame(frame);
    return true;
}

int captureReference(cv::VideoCapture *cap, Calibration *calibration, const Options &options) {
    const cv::Point center(kSensorWidth / 2, kSensorHeight / 2);
    const int warmupFrames = 10;
    const int sampleFrames = 30;
    double total = 0.0;
    int count = 0;
    std::string error;

    for (int i = 0; i < warmupFrames + sampleFrames; ++i) {
        DecodedFrame decoded;
        if (!readDecodedFrame(cap, &decoded, nullptr, &error)) {
            std::cerr << "Reference capture failed: " << error << "\n";
            return 1;
        }
        if (i >= warmupFrames) {
            total += rawRoiAverage(decoded.raw16, center, options.referenceRoi);
            count++;
        }
    }

    if (count == 0) {
        std::cerr << "Reference capture did not collect samples\n";
        return 1;
    }

    const double raw = total / count;
    calibration->references.push_back({raw, *options.captureReferenceCelsius});
    fitCalibration(*calibration);

    std::string saveError;
    if (!saveCalibration(*calibration, &saveError)) {
        std::cerr << "Failed to save calibration: " << saveError << "\n";
        return 1;
    }

    std::cout << "Captured reference: raw=" << formatDouble(raw, 3)
              << " reference=" << formatDouble(*options.captureReferenceCelsius, 2) << " C"
              << " refs=" << calibration->references.size()
              << " slope=" << std::setprecision(10) << calibration->slope
              << " offset=" << formatDouble(calibration->offsetCelsius, 3) << " C\n"
              << "Saved " << calibration->path << "\n";
    return 0;
}

void saveCalibrationOrWarn(const Calibration &calibration) {
    std::string error;
    if (!saveCalibration(calibration, &error)) {
        std::cerr << "Failed to save calibration: " << error << "\n";
    }
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parseArgs(argc, argv, &options)) {
        return 2;
    }
    if (options.help) {
        printHelp();
        return 0;
    }

    const std::string serial = serialFromDevice(options.device);
    if (options.calibrationFile.empty()) {
        options.calibrationFile = defaultCalibrationPath(serial);
    }
    Calibration calibration = loadCalibration(options.calibrationFile, serial);

    cv::VideoCapture cap;
    if (!configureCapture(&cap, options.device)) {
        return 1;
    }

    std::cout
        << "Opened " << options.device
        << " profile=mini640"
        << " width=" << cap.get(cv::CAP_PROP_FRAME_WIDTH)
        << " height=" << cap.get(cv::CAP_PROP_FRAME_HEIGHT)
        << " fourcc=" << fourccString(cap.get(cv::CAP_PROP_FOURCC))
        << "\n"
        << "Calibration file: " << calibration.path
        << (calibration.loaded ? " (loaded)" : " (default)")
        << " slope=" << std::setprecision(10) << calibration.slope
        << " offset=" << formatDouble(calibration.offsetCelsius, 3)
        << " refs=" << calibration.references.size()
        << "\n";

    if (options.captureReferenceCelsius) {
        return captureReference(&cap, &calibration, options);
    }

    ViewerState state;
    state.scale = options.scale;
    state.debugMeta = options.debugMeta;

    const std::string windowName = "Mini640 Thermal Viewer";
    cv::namedWindow(windowName, cv::WINDOW_NORMAL);
    updateWindowSize(windowName, state);
    cv::setMouseCallback(windowName, onMouse, &state);
    printHelp();

    cv::Mat lastFrame;
    for (;;) {
        cv::Mat frame;
        if (!state.freeze) {
            if (!cap.read(frame) || frame.empty()) {
                std::cerr << "Frame read failed\n";
                break;
            }
            lastFrame = frame.clone();
        } else if (!lastFrame.empty()) {
            frame = lastFrame;
        } else {
            continue;
        }

        std::string frameError;
        if (!isSupportedFrame(frame, &frameError)) {
            std::cerr << frameError << "\n";
            break;
        }

        DecodedFrame decoded = decodeFrame(frame);
        double rawMin = 0.0;
        double rawMax = 0.0;
        cv::Mat normalized = normalizeRaw(decoded.raw16, &rawMin, &rawMax);
        cv::Mat colored = colorize(normalized, state.paletteIndex);
        cv::Mat oriented = applyOrientation(colored, state);

        cv::Mat display;
        cv::resize(oriented, display, cv::Size(), state.scale, state.scale, cv::INTER_NEAREST);
        drawCrosshair(display, state);

        const uint16_t rawAtCursor = decoded.raw16.at<uint16_t>(state.cursor.y, state.cursor.x);
        const double centerRoiRaw = rawRoiAverage(decoded.raw16, state.cursor, kDefaultRoi);
        double estimateValue = 0.0;
        std::string estimateSource;
        const bool hasEstimate = mapTemperatureEstimate(
            state, calibration, decoded, centerRoiRaw, rawMin, rawMax, &estimateValue, &estimateSource);

        const double minC = calibratedRawCelsius(calibration, rawMin);
        const double maxC = calibratedRawCelsius(calibration, rawMax);
        std::vector<std::string> hud = {
            "palette: " + std::string(kPalettes[state.paletteIndex].name),
            "range: " + rangeModeLabel(state.rangeMode) + "  display: " + unitLabel(state.displayCelsius),
            "view: " + orientationLabel(state),
            "frame: " + std::to_string(frame.cols) + "x" + std::to_string(frame.rows)
                + " start=" + std::to_string(decoded.chosenStartRow),
            "raw: " + std::to_string(static_cast<int>(rawMin)) + ".." + std::to_string(static_cast<int>(rawMax))
                + "  cal: " + formatDouble(minC) + ".." + formatDouble(maxC) + " C",
            "model: slope " + formatDouble(calibration.slope, 6)
                + " off " + formatDouble(calibration.offsetCelsius, 2) + "C"
                + " refs " + std::to_string(calibration.references.size()),
        };

        if (decoded.metaValid) {
            hud.push_back(
                "meta[0..1]: " + formatDouble(decoded.meta[0] / 10.0) + " / " + formatDouble(decoded.meta[1] / 10.0)
                + "  (experimental)");
        } else {
            hud.push_back("meta: not detected");
        }

        if (hasEstimate) {
            hud.push_back(
                "cursor: (" + std::to_string(state.cursor.x) + "," + std::to_string(state.cursor.y) + ")  "
                + formatDouble(estimateValue) + " " + unitLabel(state.displayCelsius)
                + "  [" + estimateSource + "] raw=" + std::to_string(rawAtCursor));
        } else {
            hud.push_back(
                "cursor: (" + std::to_string(state.cursor.x) + "," + std::to_string(state.cursor.y)
                + ")  raw=" + std::to_string(rawAtCursor));
        }

        if (state.debugMeta) {
            std::ostringstream oss0;
            std::ostringstream oss1;
            oss0 << "row192:";
            oss1 << "row193:";
            for (int i = 0; i < 8; ++i) {
                oss0 << ' ' << std::hex << std::setw(4) << std::setfill('0') << decoded.meta[i];
                oss1 << ' ' << std::hex << std::setw(4) << std::setfill('0') << decoded.meta[16 + i];
            }
            hud.push_back(oss0.str());
            hud.push_back(oss1.str());
        }

        if (state.showHud) {
            drawTextBlock(display, hud, cv::Point(16, 16));
        }

        cv::imshow(windowName, display);
        if (cv::getWindowProperty(windowName, cv::WND_PROP_VISIBLE) < 1) {
            break;
        }

        const int key = cv::waitKey(1);
        if (key < 0) {
            continue;
        }

        switch (key & 0xFF) {
            case 'q':
                return 0;
            case 'j':
                state.paletteIndex = (state.paletteIndex + 1) % static_cast<int>(kPalettes.size());
                break;
            case 'm':
                state.paletteIndex = (state.paletteIndex + static_cast<int>(kPalettes.size()) - 1) % static_cast<int>(kPalettes.size());
                break;
            case 't':
                state.displayCelsius = !state.displayCelsius;
                break;
            case 'r':
                state.rangeMode = static_cast<RangeMode>((static_cast<int>(state.rangeMode) + 1) % 4);
                break;
            case '8':
                state.rotationQuarterTurns = (state.rotationQuarterTurns + 1) % 4;
                updateWindowSize(windowName, state);
                break;
            case '9':
                state.rotationQuarterTurns = (state.rotationQuarterTurns + 3) % 4;
                updateWindowSize(windowName, state);
                break;
            case 'x':
                state.flipHorizontal = !state.flipHorizontal;
                break;
            case 'y':
                state.flipVertical = !state.flipVertical;
                break;
            case '-':
            case '_':
                calibration.offsetCelsius -= 0.5;
                saveCalibrationOrWarn(calibration);
                break;
            case '=':
            case '+':
                calibration.offsetCelsius += 0.5;
                saveCalibrationOrWarn(calibration);
                break;
            case '0':
                fitCalibration(calibration);
                saveCalibrationOrWarn(calibration);
                break;
            case '[':
                state.scale = std::max(1, state.scale - 1);
                updateWindowSize(windowName, state);
                break;
            case ']':
                state.scale = std::min(8, state.scale + 1);
                updateWindowSize(windowName, state);
                break;
            case 'h':
                state.showHud = !state.showHud;
                break;
            case 'd':
                state.debugMeta = !state.debugMeta;
                break;
            case ' ':
                state.freeze = !state.freeze;
                break;
            case 'f':
                state.fullscreen = !state.fullscreen;
                cv::setWindowProperty(
                    windowName,
                    cv::WND_PROP_FULLSCREEN,
                    state.fullscreen ? cv::WINDOW_FULLSCREEN : cv::WINDOW_NORMAL);
                break;
            default:
                break;
        }
    }

    return 0;
}
