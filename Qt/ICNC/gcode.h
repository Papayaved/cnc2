#ifndef GCODE_H
#define GCODE_H

#include <string>
#include <list>
#include <QTextEdit>
#include <deque>
#include <algorithm>
#include <vector>
#include <map>

#include "gpoint.h"
#include "gframe.h"
#include "contour_list.h"

enum class LAYER_T : uint8_t {BOT, TOP};

struct GCodeSettings {
    struct {
        uint8_t uv:1;
        uint8_t L:1;
        uint8_t H:1;
        uint8_t D:1;
    } valid;
    bool uv_ena;
    double L, H, D;

    GCodeSettings() : valid({0,0,0,0}), uv_ena(false), L(0), H(0), D(0) {}
    bool isUV() const {
        return valid.uv && valid.L && valid.H && valid.D && L > 0 && H > 0 && D > 0 && H + D < L;
    }

    void clear() { *this = GCodeSettings(); }
};

class GCode {
    constexpr static const double M_PRECISION = PRECISION; // mm

    const QColor XY_INCUT_COLOR = Qt::GlobalColor::blue;
    const QColor XY_COLOR = Qt::GlobalColor::darkBlue;
    const QColor XY_LIT_COLOR = Qt::GlobalColor::magenta;

    const QColor UV_INCUT_COLOR = Qt::GlobalColor::red;
    const QColor UV_COLOR = Qt::GlobalColor::darkRed;
    const QColor UV_LIT_COLOR = Qt::GlobalColor::darkYellow;

    std::vector<GFrame> frames;

    std::string m_error;

    static void pointsOffset(std::vector<GPoint>& pts, const GPoint& pt);

    void append(const GEntity& entity);
    void append(const GEntity& entity0, const GEntity& entity1);
    void append(const GEntity& entity0, const GEntity& entity1, const GEntity& entity2, const GEntity& entity3);
    void append(const GFrame& frame);
    void push_back(const GeneratorMode& genMode);

    static void addLine(Dxf& cnt, const GCommand& cmd, bool inc_ena, const fpoint_t& offset, double& X, double& Y, LAYER_T layer = LAYER_T::BOT);
    static void addArc (Dxf& cnt, const GCommand& cmd, bool inc_ena, const fpoint_t& offset, double& X, double& Y, bool ccw, LAYER_T layer = LAYER_T::BOT);

    void addG2G2(fpoint_t& A, const fpoint_t& B, fpoint_t& A2, const fpoint_t& B2, const fpoint_t& C);

public:
    GCode() {}
    GCode(const GCode& other) : frames(other.frames) { }
    GCode(GCode&& other) : frames(std::move(other.frames))  { }

    GCode& operator=(const GCode& other) noexcept;
    GCode& operator=(GCode&& other) noexcept;

    int parse(const std::string& gcode);
    bool generate(const cut_t& cut, const std::deque<ContourPair>& contours);
    bool generate(double d_top, double d_bot, double L, double H, double T, double cutline, AXIS axis);

    void removeComments();
    void gNormalize();
    void RtoIJ();
    void normalize();

    std::deque<ContourPair> getContours(std::vector<std::pair<size_t, size_t>>* const map = nullptr) const;

    std::string toText() const;
    std::list<std::string> toFrameList();

    bool getEntityNumber(size_t frameNumber, size_t& countourNumber, size_t& entityNumber) const;


//    GCode noComment() const;
    void clear();
    bool empty() const;
    // move, rotate

    GCodeSettings getSettings() const;

    const std::string& lastError() const { return m_error; }

    static std::list<std::string> splitTextByStrings(const std::string& text);
    static bool CompareFrames(const std::list<std::string>& ref, const std::list<std::string>& cmp, std::string& error_text);
    static std::string toText(std::list<std::string>& gframes);
    static std::pair<std::pair<double, double>, std::pair<double, double>> MinMaxPlotAxis(const std::vector<GPoint>& pts);
};

#endif // GCODE_H
