#include "MainWindow.h"

#include "BacktestEngine.h"
#include "Indicators.h"
#include "Resource.h"

#include <algorithm>
#include <commctrl.h>
#include <cstdlib>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <windowsx.h>

namespace {

constexpr int IDC_BTN_START = 2001;
constexpr int IDC_BTN_PAUSE = 2002;
constexpr int IDC_BTN_RESET = 2003;
constexpr int IDC_BTN_OPTIMIZE = 2004;
constexpr int IDC_COMBO_STRATEGY = 2005;
constexpr int IDC_LIST_ORDERS = 2006;
constexpr int IDC_LIST_TRADES = 2007;
constexpr int IDC_LIST_LOGS = 2008;
constexpr int IDC_EDIT_CASH = 2009;
constexpr int IDC_EDIT_SHORT = 2010;
constexpr int IDC_EDIT_LONG = 2011;
constexpr int IDC_COMBO_PERIOD = 2012;
constexpr int IDC_COMBO_SPEED = 2013;
constexpr int IDC_COMBO_INDICATOR = 2014;

constexpr COLORREF CLR_BG = RGB(239, 239, 239);
constexpr COLORREF CLR_PANEL = RGB(255, 255, 255);
constexpr COLORREF CLR_PANEL_2 = RGB(248, 248, 248);
constexpr COLORREF CLR_BORDER = RGB(160, 160, 160);
constexpr COLORREF CLR_TEXT = RGB(20, 20, 20);
constexpr COLORREF CLR_MUTED = RGB(96, 96, 96);
constexpr COLORREF CLR_GRID = RGB(210, 210, 210);
constexpr COLORREF CLR_RED = RGB(255, 30, 30);
constexpr COLORREF CLR_GREEN = RGB(0, 140, 0);
constexpr COLORREF CLR_BLUE = RGB(0, 92, 255);
constexpr COLORREF CLR_YELLOW = RGB(236, 150, 0);
constexpr COLORREF CLR_MAGENTA = RGB(255, 0, 255);
constexpr COLORREF CLR_ORANGE = RGB(238, 88, 0);
constexpr COLORREF CLR_GRAY_BAR = RGB(155, 155, 155);

HINSTANCE g_instance = nullptr;
const wchar_t* kWindowClass = L"StockSystemWin32Class";

struct Rects {
    RECT quote{};
    RECT tabs{};
    RECT chart{};
    RECT volume{};
    RECT indicator{};
    RECT side{};
    RECT bottom{};
};

struct AppState {
    HWND hStart = nullptr;
    HWND hPause = nullptr;
    HWND hReset = nullptr;
    HWND hOptimize = nullptr;
    HWND hStrategy = nullptr;
    HWND hPeriod = nullptr;
    HWND hSpeed = nullptr;
    HWND hIndicator = nullptr;
    HWND hOrders = nullptr;
    HWND hTrades = nullptr;
    HWND hLogs = nullptr;
    HWND hCash = nullptr;
    HWND hShort = nullptr;
    HWND hLong = nullptr;
    HWND hStatus = nullptr;
    Rects rects;
    int visibleBars = 110;
    int periodFactor = 1;
    int selectedBar = -1;
    int indicatorMode = 1;
    std::wstring chartHint = L"Synthetic market. Select period, speed, MA windows, and indicator.";
    HBRUSH bgBrush = CreateSolidBrush(CLR_BG);
    HBRUSH panelBrush = CreateSolidBrush(CLR_PANEL);
    HBRUSH editBrush = CreateSolidBrush(CLR_PANEL_2);
    HFONT uiFont = nullptr;
    SimulationEngine engine;
    UiSnapshot snapshot;
    size_t renderedOrderCount = static_cast<size_t>(-1);
    size_t renderedTradeCount = static_cast<size_t>(-1);
    size_t renderedLogCount = static_cast<size_t>(-1);

    ~AppState()
    {
        if (bgBrush) {
            DeleteObject(bgBrush);
        }
        if (panelBrush) {
            DeleteObject(panelBrush);
        }
        if (editBrush) {
            DeleteObject(editBrush);
        }
        if (uiFont) {
            DeleteObject(uiFont);
        }
    }
};

std::unique_ptr<AppState> g_app;

std::wstring money(double value)
{
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << value;
    return ss.str();
}

std::wstring percent(double value)
{
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << value * 100.0 << L"%";
    return ss.str();
}

double readDouble(HWND edit, double fallback)
{
    wchar_t buffer[64]{};
    GetWindowTextW(edit, buffer, 64);
    wchar_t* end = nullptr;
    const double value = wcstod(buffer, &end);
    return end == buffer ? fallback : value;
}

int readInt(HWND edit, int fallback)
{
    wchar_t buffer[64]{};
    GetWindowTextW(edit, buffer, 64);
    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 10);
    return end == buffer ? fallback : static_cast<int>(value);
}

int comboSelection(HWND combo, int fallback)
{
    const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return sel == CB_ERR ? fallback : static_cast<int>(sel);
}

int selectedPeriodFactor()
{
    static constexpr int factors[] = { 1, 5, 15, 30, 60, 240, 1200 };
    return factors[std::clamp(comboSelection(g_app->hPeriod, 0), 0, 6)];
}

int selectedReplayDelay()
{
    static constexpr int delays[] = { 220, 90, 30, 10 };
    return delays[std::clamp(comboSelection(g_app->hSpeed, 1), 0, 3)];
}

std::wstring periodName()
{
    static constexpr const wchar_t* names[] = { L"1m", L"5m", L"15m", L"30m", L"60m", L"Day", L"Week" };
    return names[std::clamp(comboSelection(g_app->hPeriod, 0), 0, 6)];
}

void addListLine(HWND listBox, const std::wstring& text)
{
    SendMessageW(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void refreshLists(HWND window)
{
    if (!g_app) {
        return;
    }
    g_app->snapshot = g_app->engine.snapshot();
    g_app->periodFactor = selectedPeriodFactor();
    g_app->indicatorMode = comboSelection(g_app->hIndicator, 1);

    if (g_app->renderedOrderCount != g_app->snapshot.orders.size()) {
        SendMessageW(g_app->hOrders, WM_SETREDRAW, FALSE, 0);
        SendMessageW(g_app->hOrders, LB_RESETCONTENT, 0, 0);
        for (auto it = g_app->snapshot.orders.rbegin(); it != g_app->snapshot.orders.rend(); ++it) {
            std::wstringstream ss;
            ss << L"#" << it->order.id << L" " << (it->order.isBuy ? L"BUY " : L"SELL ")
               << it->order.volume << L" @ " << money(it->order.price)
               << L" | " << it->status;
            if (it->filledVolume > 0) {
                ss << L" " << it->filledVolume << L" @ " << money(it->averageFillPrice);
            }
            addListLine(g_app->hOrders, ss.str());
        }
        SendMessageW(g_app->hOrders, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_app->hOrders, nullptr, TRUE);
        g_app->renderedOrderCount = g_app->snapshot.orders.size();
    }

    if (g_app->renderedTradeCount != g_app->snapshot.trades.size()) {
        SendMessageW(g_app->hTrades, WM_SETREDRAW, FALSE, 0);
        SendMessageW(g_app->hTrades, LB_RESETCONTENT, 0, 0);
        for (auto it = g_app->snapshot.trades.rbegin(); it != g_app->snapshot.trades.rend(); ++it) {
            std::wstringstream ss;
            ss << L"#" << it->id << L" order #" << it->orderId << L" "
               << (it->isBuy ? L"BUY " : L"SELL ")
               << it->volume << L" @ " << money(it->price);
            addListLine(g_app->hTrades, ss.str());
        }
        SendMessageW(g_app->hTrades, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_app->hTrades, nullptr, TRUE);
        g_app->renderedTradeCount = g_app->snapshot.trades.size();
    }

    if (g_app->renderedLogCount != g_app->snapshot.logs.size()) {
        SendMessageW(g_app->hLogs, WM_SETREDRAW, FALSE, 0);
        SendMessageW(g_app->hLogs, LB_RESETCONTENT, 0, 0);
        for (auto it = g_app->snapshot.logs.rbegin(); it != g_app->snapshot.logs.rend(); ++it) {
            addListLine(g_app->hLogs, *it);
        }
        SendMessageW(g_app->hLogs, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_app->hLogs, nullptr, TRUE);
        g_app->renderedLogCount = g_app->snapshot.logs.size();
    }

    std::wstringstream status;
    status << L"TEST.SH " << periodName()
           << L" | Price " << money(g_app->snapshot.lastPrice)
           << L" | Cash " << money(g_app->snapshot.account.cash)
           << L" | Position " << g_app->snapshot.account.position
           << L" | Equity " << money(g_app->snapshot.account.equity)
           << L" | MaxDD " << percent(g_app->snapshot.account.maxDrawdown);
    SetWindowTextW(g_app->hStatus, status.str().c_str());
    InvalidateRect(window, nullptr, FALSE);
}

void startBacktest(HWND window)
{
    const double cash = readDouble(g_app->hCash, 100000.0);
    const int shortW = std::max(1, readInt(g_app->hShort, 5));
    const int longW = std::max(shortW + 1, readInt(g_app->hLong, 30));
    g_app->engine.configure(cash, shortW, longW);
    g_app->engine.setReplayDelay(selectedReplayDelay());
    g_app->engine.start(window);
    refreshLists(window);
}

void setChildFont(HWND child)
{
    if (g_app && g_app->uiFont) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->uiFont), TRUE);
    }
}

void layout(HWND window)
{
    if (!g_app) {
        return;
    }

    RECT rc{};
    GetClientRect(window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int top = 8;
    const int quoteHeight = 62;
    const int tabHeight = 36;
    const int sideWidth = 300;
    const int bottomHeight = 150;
    const int gap = 10;
    const int chartBottom = height - bottomHeight - gap;
    const int plotTop = top + quoteHeight + tabHeight + gap;
    const int mainWidth = width - sideWidth - gap * 3;

    g_app->rects.quote = { gap, top, width - gap, top + quoteHeight };
    g_app->rects.tabs = { gap, top + quoteHeight + 4, width - sideWidth - gap * 2, top + quoteHeight + tabHeight };
    g_app->rects.chart = { 82, plotTop + 18, mainWidth, plotTop + (chartBottom - plotTop) * 55 / 100 };
    g_app->rects.volume = { 82, g_app->rects.chart.bottom + 28, mainWidth, g_app->rects.chart.bottom + 128 };
    g_app->rects.indicator = { 82, g_app->rects.volume.bottom + 28, mainWidth, chartBottom };
    g_app->rects.side = { width - sideWidth - gap, top + quoteHeight + 4, width - gap, chartBottom };
    g_app->rects.bottom = { 14, height - bottomHeight, width - 14, height - 32 };

    MoveWindow(g_app->hPeriod, 96, g_app->rects.tabs.top + 6, 92, 200, TRUE);
    MoveWindow(g_app->hSpeed, 266, g_app->rects.tabs.top + 6, 112, 200, TRUE);
    MoveWindow(g_app->hIndicator, 468, g_app->rects.tabs.top + 6, 122, 200, TRUE);

    const int sx = g_app->rects.side.left;
    const int sy = g_app->rects.side.bottom - 276;
    const int fieldX = sx + 18;
    const int fieldWidth = sideWidth - 36;
    MoveWindow(g_app->hStrategy, fieldX, sy + 48, fieldWidth, 26, TRUE);
    MoveWindow(g_app->hCash, fieldX, sy + 112, fieldWidth, 24, TRUE);
    MoveWindow(g_app->hShort, fieldX, sy + 176, 112, 24, TRUE);
    MoveWindow(g_app->hLong, fieldX + 136, sy + 176, 112, 24, TRUE);
    MoveWindow(g_app->hStart, fieldX, sy + 232, 58, 24, TRUE);
    MoveWindow(g_app->hPause, fieldX + 68, sy + 232, 58, 24, TRUE);
    MoveWindow(g_app->hReset, fieldX + 136, sy + 232, 58, 24, TRUE);
    MoveWindow(g_app->hOptimize, fieldX + 204, sy + 232, 64, 24, TRUE);

    const int third = (g_app->rects.bottom.right - g_app->rects.bottom.left - 20) / 3;
    MoveWindow(g_app->hOrders, g_app->rects.bottom.left, g_app->rects.bottom.top + 24, third, bottomHeight - 60, TRUE);
    MoveWindow(g_app->hTrades, g_app->rects.bottom.left + third + 10, g_app->rects.bottom.top + 24, third, bottomHeight - 60, TRUE);
    MoveWindow(g_app->hLogs, g_app->rects.bottom.left + (third + 10) * 2, g_app->rects.bottom.top + 24, third, bottomHeight - 60, TRUE);
    MoveWindow(g_app->hStatus, 0, height - 24, width, 24, TRUE);
}

void createControls(HWND window)
{
    auto menuId = [](int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); };
    g_app->uiFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    g_app->hStart = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, menuId(IDC_BTN_START), g_instance, nullptr);
    g_app->hPause = CreateWindowW(L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, menuId(IDC_BTN_PAUSE), g_instance, nullptr);
    g_app->hReset = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, menuId(IDC_BTN_RESET), g_instance, nullptr);
    g_app->hOptimize = CreateWindowW(L"BUTTON", L"Optimize", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, menuId(IDC_BTN_OPTIMIZE), g_instance, nullptr);
    g_app->hPeriod = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0, 0, window, menuId(IDC_COMBO_PERIOD), g_instance, nullptr);
    for (const wchar_t* text : { L"1m", L"5m", L"15m", L"30m", L"60m", L"Day", L"Week" }) {
        SendMessageW(g_app->hPeriod, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    }
    SendMessageW(g_app->hPeriod, CB_SETCURSEL, 0, 0);

    g_app->hSpeed = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0, 0, window, menuId(IDC_COMBO_SPEED), g_instance, nullptr);
    for (const wchar_t* text : { L"Slow", L"Normal", L"Fast", L"Turbo" }) {
        SendMessageW(g_app->hSpeed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    }
    SendMessageW(g_app->hSpeed, CB_SETCURSEL, 1, 0);

    g_app->hIndicator = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0, 0, window, menuId(IDC_COMBO_INDICATOR), g_instance, nullptr);
    for (const wchar_t* text : { L"Volume", L"RSI", L"KDJ", L"MACD" }) {
        SendMessageW(g_app->hIndicator, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    }
    SendMessageW(g_app->hIndicator, CB_SETCURSEL, 1, 0);

    g_app->hStrategy = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0, 0, window, menuId(IDC_COMBO_STRATEGY), g_instance, nullptr);
    SendMessageW(g_app->hStrategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Moving Average Crossover"));
    SendMessageW(g_app->hStrategy, CB_SETCURSEL, 0, 0);

    g_app->hCash = CreateWindowW(L"EDIT", L"100000", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, window, menuId(IDC_EDIT_CASH), g_instance, nullptr);
    g_app->hShort = CreateWindowW(L"EDIT", L"5", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, window, menuId(IDC_EDIT_SHORT), g_instance, nullptr);
    g_app->hLong = CreateWindowW(L"EDIT", L"30", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, window, menuId(IDC_EDIT_LONG), g_instance, nullptr);
    g_app->hOrders = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, 0, 0, 0, 0, window, menuId(IDC_LIST_ORDERS), g_instance, nullptr);
    g_app->hTrades = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, 0, 0, 0, 0, window, menuId(IDC_LIST_TRADES), g_instance, nullptr);
    g_app->hLogs = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, 0, 0, 0, 0, window, menuId(IDC_LIST_LOGS), g_instance, nullptr);
    g_app->hStatus = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE | SS_SUNKEN, 0, 0, 0, 0, window, nullptr, g_instance, nullptr);

    for (HWND child : { g_app->hStart, g_app->hPause, g_app->hReset, g_app->hOptimize,
        g_app->hPeriod, g_app->hSpeed, g_app->hIndicator, g_app->hStrategy,
        g_app->hCash, g_app->hShort, g_app->hLong, g_app->hOrders, g_app->hTrades, g_app->hLogs, g_app->hStatus }) {
        setChildFont(child);
    }
    layout(window);
}

void drawSeries(HDC dc, const RECT& rect, const std::vector<double>& values, COLORREF color)
{
    if (values.size() < 2) {
        return;
    }
    const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    const double minValue = *minIt;
    const double maxValue = *maxIt;
    const double span = std::max(0.0001, maxValue - minValue);
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    auto pointAt = [&](size_t i) {
        const double xRatio = static_cast<double>(i) / (values.size() - 1);
        const double yRatio = (values[i] - minValue) / span;
        return POINT{
            rect.left + static_cast<LONG>(xRatio * (rect.right - rect.left)),
            rect.bottom - static_cast<LONG>(yRatio * (rect.bottom - rect.top))
        };
    };
    POINT first = pointAt(0);
    MoveToEx(dc, first.x, first.y, nullptr);
    for (size_t i = 1; i < values.size(); ++i) {
        POINT p = pointAt(i);
        LineTo(dc, p.x, p.y);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

std::vector<MarketBar> aggregateBars(const std::vector<MarketBar>& source, int factor)
{
    if (factor <= 1 || source.empty()) {
        return source;
    }

    std::vector<MarketBar> out;
    for (size_t i = 0; i < source.size(); i += static_cast<size_t>(factor)) {
        const size_t end = std::min(source.size(), i + static_cast<size_t>(factor));
        MarketBar bar = source[i];
        bar.open = source[i].open;
        bar.high = source[i].high;
        bar.low = source[i].low;
        bar.close = source[end - 1].close;
        bar.price = bar.close;
        bar.volume = 0;
        bar.timestamp = source[end - 1].timestamp;
        for (size_t j = i; j < end; ++j) {
            bar.high = std::max(bar.high, source[j].high);
            bar.low = std::min(bar.low, source[j].low);
            bar.volume += source[j].volume;
        }
        out.push_back(bar);
    }
    return out;
}

void drawFrameAndGrid(HDC dc, const RECT& rect, int horizontal = 4)
{
    HBRUSH white = CreateSolidBrush(CLR_PANEL);
    FillRect(dc, &rect, white);
    DeleteObject(white);
    HBRUSH border = CreateSolidBrush(CLR_BORDER);
    FrameRect(dc, &rect, border);
    DeleteObject(border);

    HPEN gridPen = CreatePen(PS_DOT, 1, CLR_GRID);
    HGDIOBJ oldPen = SelectObject(dc, gridPen);
    for (int i = 1; i < horizontal; ++i) {
        const int y = rect.top + (rect.bottom - rect.top) * i / horizontal;
        MoveToEx(dc, rect.left, y, nullptr);
        LineTo(dc, rect.right, y);
    }
    for (int i = 1; i < 4; ++i) {
        const int x = rect.left + (rect.right - rect.left) * i / 4;
        MoveToEx(dc, x, rect.top, nullptr);
        LineTo(dc, x, rect.bottom);
    }
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);
}

void drawLineWithScale(HDC dc, const RECT& rect, const std::vector<double>& values, double minValue, double maxValue, COLORREF color, int width = 1)
{
    if (values.size() < 2) {
        return;
    }
    const double span = std::max(0.0001, maxValue - minValue);
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    auto pointAt = [&](size_t i) {
        const double xRatio = static_cast<double>(i) / static_cast<double>(values.size() - 1);
        const double yRatio = (values[i] - minValue) / span;
        return POINT{
            rect.left + static_cast<LONG>(xRatio * (rect.right - rect.left)),
            rect.bottom - static_cast<LONG>(yRatio * (rect.bottom - rect.top))
        };
    };
    bool hasPrevious = false;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) {
            hasPrevious = false;
            continue;
        }
        POINT p = pointAt(i);
        if (!hasPrevious) {
            MoveToEx(dc, p.x, p.y, nullptr);
            hasPrevious = true;
        } else {
            LineTo(dc, p.x, p.y);
        }
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void drawValueLabel(HDC dc, int x, int y, const wchar_t* name, double value, COLORREF color)
{
    std::wstringstream ss;
    ss << name << L": " << std::fixed << std::setprecision(2) << value;
    SetTextColor(dc, color);
    TextOutW(dc, x, y, ss.str().c_str(), static_cast<int>(ss.str().size()));
}

void drawYAxisLabels(HDC dc, const RECT& rect, double minValue, double maxValue, int decimals)
{
    SetTextColor(dc, CLR_TEXT);
    for (int i = 0; i <= 4; ++i) {
        const double value = maxValue - (maxValue - minValue) * i / 4.0;
        const int y = rect.top + (rect.bottom - rect.top) * i / 4 - 8;
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(decimals) << value;
        TextOutW(dc, rect.left - 76, y, ss.str().c_str(), static_cast<int>(ss.str().size()));
    }
}

void drawTimeLabels(HDC dc, const RECT& rect, const std::vector<MarketBar>& bars)
{
    if (bars.empty()) {
        return;
    }
    SetTextColor(dc, CLR_TEXT);
    for (int i = 0; i <= 4; ++i) {
        const size_t index = std::min(bars.size() - 1, static_cast<size_t>(i * (bars.size() - 1) / 4));
        const int x = rect.left + (rect.right - rect.left) * i / 4 - 18;
        std::wstringstream ss;
        ss << L"T+" << bars[index].timestamp;
        TextOutW(dc, x, rect.bottom + 5, ss.str().c_str(), static_cast<int>(ss.str().size()));
    }
}

void drawKLine(HDC dc, const RECT& rect, const std::vector<MarketBar>& allBars, int visibleBars)
{
    drawFrameAndGrid(dc, rect, 5);
    if (allBars.empty()) {
        SetTextColor(dc, CLR_MUTED);
        DrawTextW(dc, L"No market data yet. Press Start or Space.", -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const auto aggregated = aggregateBars(allBars, g_app->periodFactor);
    const size_t count = std::min(static_cast<size_t>(std::max(20, visibleBars)), aggregated.size());
    if (count == 0) {
        return;
    }
    const auto first = aggregated.end() - static_cast<std::ptrdiff_t>(count);
    std::vector<MarketBar> bars(first, aggregated.end());
    std::vector<double> closes;
    closes.reserve(bars.size());

    double minPrice = bars.front().low;
    double maxPrice = bars.front().high;
    for (const auto& bar : bars) {
        minPrice = std::min(minPrice, bar.low);
        maxPrice = std::max(maxPrice, bar.high);
        closes.push_back(bar.close);
    }
    drawYAxisLabels(dc, rect, minPrice, maxPrice, 2);
    drawTimeLabels(dc, rect, bars);
    const auto ma5 = movingAverageParallel(closes, 5);
    const auto ma10 = movingAverageParallel(closes, 10);
    const auto ma20 = movingAverageParallel(closes, 20);
    const auto ma60 = movingAverageParallel(closes, 60);

    const double span = std::max(0.0001, maxPrice - minPrice);
    auto yOf = [&](double price) {
        const double ratio = (price - minPrice) / span;
        return rect.bottom - static_cast<int>(ratio * (rect.bottom - rect.top));
    };

    const int width = std::max(3, static_cast<int>((rect.right - rect.left) / static_cast<LONG>(bars.size())));
    const int bodyWidth = std::max(3, width - 3);
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& bar = bars[i];
        const int x = rect.left + static_cast<int>((i + 0.5) * (rect.right - rect.left) / bars.size());
        const bool up = bar.close >= bar.open;
        const COLORREF color = up ? CLR_RED : CLR_GREEN;
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ prevPen = SelectObject(dc, pen);
        HGDIOBJ prevBrush = SelectObject(dc, brush);
        MoveToEx(dc, x, yOf(bar.high), nullptr);
        LineTo(dc, x, yOf(bar.low));
        RECT body{
            x - bodyWidth / 2,
            std::min(yOf(bar.open), yOf(bar.close)),
            x + bodyWidth / 2,
            std::max(yOf(bar.open), yOf(bar.close)) + 1
        };
        Rectangle(dc, body.left, body.top, body.right, body.bottom);
        SelectObject(dc, prevBrush);
        SelectObject(dc, prevPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    drawLineWithScale(dc, rect, ma5, minPrice, maxPrice, CLR_YELLOW, 2);
    drawLineWithScale(dc, rect, ma10, minPrice, maxPrice, CLR_BLUE, 1);
    drawLineWithScale(dc, rect, ma20, minPrice, maxPrice, CLR_MAGENTA, 1);
    drawLineWithScale(dc, rect, ma60, minPrice, maxPrice, CLR_GREEN, 1);

    const auto lastFinite = [](const std::vector<double>& values) {
        for (auto it = values.rbegin(); it != values.rend(); ++it) {
            if (std::isfinite(*it)) {
                return *it;
            }
        }
        return 0.0;
    };
    drawValueLabel(dc, rect.left + 8, rect.top - 20, L"MA5", lastFinite(ma5), CLR_BLUE);
    drawValueLabel(dc, rect.left + 112, rect.top - 20, L"MA10", lastFinite(ma10), CLR_YELLOW);
    drawValueLabel(dc, rect.left + 230, rect.top - 20, L"MA20", lastFinite(ma20), CLR_MAGENTA);
    drawValueLabel(dc, rect.left + 350, rect.top - 20, L"MA60", lastFinite(ma60), CLR_GREEN);
}

void drawVolumePanel(HDC dc, const RECT& rect, const std::vector<MarketBar>& allBars, int visibleBars)
{
    drawFrameAndGrid(dc, rect, 3);
    const auto aggregated = aggregateBars(allBars, g_app->periodFactor);
    const size_t count = std::min(static_cast<size_t>(std::max(20, visibleBars)), aggregated.size());
    if (count == 0) {
        return;
    }
    const auto first = aggregated.end() - static_cast<std::ptrdiff_t>(count);
    std::vector<MarketBar> bars(first, aggregated.end());
    int maxVolume = 1;
    std::vector<double> volumes;
    volumes.reserve(bars.size());
    for (const auto& bar : bars) {
        maxVolume = std::max(maxVolume, bar.volume);
        volumes.push_back(static_cast<double>(bar.volume));
    }
    drawYAxisLabels(dc, rect, 0.0, static_cast<double>(maxVolume), 0);
    drawTimeLabels(dc, rect, bars);
    const auto ma5 = movingAverageParallel(volumes, 5);
    const auto ma10 = movingAverageParallel(volumes, 10);
    const int width = std::max(2, static_cast<int>((rect.right - rect.left) / static_cast<LONG>(bars.size())));
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& bar = bars[i];
        const int x = rect.left + static_cast<int>(i * (rect.right - rect.left) / bars.size());
        const int h = static_cast<int>((rect.bottom - rect.top - 16) * static_cast<double>(bar.volume) / maxVolume);
        RECT body{ x + 1, rect.bottom - h, x + std::max(2, width - 1), rect.bottom - 1 };
        HBRUSH brush = CreateSolidBrush(bar.close >= bar.open ? CLR_RED : CLR_GREEN);
        FrameRect(dc, &body, brush);
        if (bar.close < bar.open) {
            FillRect(dc, &body, brush);
        }
        DeleteObject(brush);
    }
    drawLineWithScale(dc, rect, ma5, 0.0, static_cast<double>(maxVolume), CLR_GRAY_BAR, 1);
    drawLineWithScale(dc, rect, ma10, 0.0, static_cast<double>(maxVolume), CLR_MAGENTA, 1);
    SetTextColor(dc, CLR_MAGENTA);
    TextOutW(dc, rect.left + 8, rect.top + 4, L"VOL", 3);
}

void drawIndicatorPanel(HDC dc, const RECT& rect, const std::vector<MarketBar>& allBars, int visibleBars)
{
    drawFrameAndGrid(dc, rect, 3);
    const auto aggregated = aggregateBars(allBars, g_app->periodFactor);
    const size_t count = std::min(static_cast<size_t>(std::max(20, visibleBars)), aggregated.size());
    if (count == 0) {
        return;
    }
    const auto first = aggregated.end() - static_cast<std::ptrdiff_t>(count);
    std::vector<MarketBar> bars(first, aggregated.end());
    std::vector<double> highs;
    std::vector<double> lows;
    std::vector<double> closes;
    highs.reserve(bars.size());
    lows.reserve(bars.size());
    closes.reserve(bars.size());
    for (const auto& bar : bars) {
        highs.push_back(bar.high);
        lows.push_back(bar.low);
        closes.push_back(bar.close);
    }
    drawTimeLabels(dc, rect, bars);

    SetTextColor(dc, CLR_MUTED);
    if (g_app->indicatorMode == 0) {
        TextOutW(dc, rect.left + 8, rect.top + 4, L"Volume selected above", 21);
        return;
    }
    if (g_app->indicatorMode == 1) {
        drawYAxisLabels(dc, rect, 0.0, 100.0, 0);
        const auto rsi6 = rsi(closes, 6);
        const auto rsi12 = rsi(closes, 12);
        const auto rsi24 = rsi(closes, 24);
        drawLineWithScale(dc, rect, rsi6, 0.0, 100.0, CLR_GRAY_BAR, 1);
        drawLineWithScale(dc, rect, rsi12, 0.0, 100.0, CLR_YELLOW, 1);
        drawLineWithScale(dc, rect, rsi24, 0.0, 100.0, CLR_MAGENTA, 1);
        TextOutW(dc, rect.left + 8, rect.top + 4, L"RSI6 / RSI12 / RSI24", 20);
    } else if (g_app->indicatorMode == 2) {
        drawYAxisLabels(dc, rect, 0.0, 100.0, 0);
        const auto values = kdj(highs, lows, closes, 9);
        drawLineWithScale(dc, rect, values.k, 0.0, 100.0, CLR_GRAY_BAR, 1);
        drawLineWithScale(dc, rect, values.d, 0.0, 100.0, CLR_YELLOW, 1);
        drawLineWithScale(dc, rect, values.j, -20.0, 120.0, CLR_MAGENTA, 1);
        TextOutW(dc, rect.left + 8, rect.top + 4, L"KDJ K / D / J", 13);
    } else {
        const auto values = macd(closes);
        double minValue = 0.0;
        double maxValue = 0.0;
        for (double v : values.hist) {
            if (std::isfinite(v)) {
                minValue = std::min(minValue, v);
                maxValue = std::max(maxValue, v);
            }
        }
        drawYAxisLabels(dc, rect, minValue, maxValue, 2);
        const double span = std::max(0.0001, maxValue - minValue);
        const int zeroY = rect.bottom - static_cast<int>((0.0 - minValue) / span * (rect.bottom - rect.top));
        for (size_t i = 0; i < values.hist.size(); ++i) {
            if (!std::isfinite(values.hist[i])) {
                continue;
            }
            const int x = rect.left + static_cast<int>(i * (rect.right - rect.left) / values.hist.size());
            const int y = rect.bottom - static_cast<int>((values.hist[i] - minValue) / span * (rect.bottom - rect.top));
            HPEN pen = CreatePen(PS_SOLID, 2, values.hist[i] >= 0.0 ? CLR_RED : CLR_GREEN);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, x, zeroY, nullptr);
            LineTo(dc, x, y);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        drawLineWithScale(dc, rect, values.dif, minValue, maxValue, CLR_BLUE, 1);
        drawLineWithScale(dc, rect, values.dea, minValue, maxValue, CLR_YELLOW, 1);
        TextOutW(dc, rect.left + 8, rect.top + 4, L"MACD DIF / DEA / BAR", 19);
    }
}

void text(HDC dc, int x, int y, const std::wstring& value, COLORREF color = CLR_TEXT)
{
    SetTextColor(dc, color);
    TextOutW(dc, x, y, value.c_str(), static_cast<int>(value.size()));
}

void drawQuoteBar(HDC dc)
{
    RECT quote = g_app->rects.quote;
    HBRUSH bg = CreateSolidBrush(RGB(247, 247, 247));
    FillRect(dc, &quote, bg);
    DeleteObject(bg);

    const auto& s = g_app->snapshot;
    const double change = s.bars.size() >= 2 ? s.lastPrice - s.bars[s.bars.size() - 2].close : 0.0;
    const double pct = s.bars.size() >= 2 ? change / std::max(1.0, s.bars[s.bars.size() - 2].close) : 0.0;
    const COLORREF priceColor = change >= 0.0 ? CLR_RED : CLR_GREEN;

    std::wstringstream title;
    title << L"TEST.SH 000001   " << money(s.lastPrice) << (change >= 0.0 ? L" UP" : L" DOWN");
    text(dc, quote.left + 12, quote.top + 20, title.str(), priceColor);

    const int x0 = quote.left + 430;
    const int y0 = quote.top + 10;
    std::vector<std::wstring> rows;
    rows.push_back(L"Open: " + (s.bars.empty() ? L"--" : money(s.bars.back().open)));
    rows.push_back(L"High: " + (s.bars.empty() ? L"--" : money(s.bars.back().high)));
    rows.push_back(L"Change: " + percent(pct));
    rows.push_back(L"Turnover: " + money(std::abs(change) * 10000.0));
    rows.push_back(L"Volume: " + (s.bars.empty() ? L"--" : std::to_wstring(s.bars.back().volume)));
    rows.push_back(L"Position: " + std::to_wstring(s.account.position));
    rows.push_back(L"Prev: " + (s.bars.size() >= 2 ? money(s.bars[s.bars.size() - 2].close) : L"--"));
    rows.push_back(L"Low: " + (s.bars.empty() ? L"--" : money(s.bars.back().low)));
    rows.push_back(L"Equity: " + money(s.account.equity));
    rows.push_back(L"Cash: " + money(s.account.cash));
    rows.push_back(L"MaxDD: " + percent(s.account.maxDrawdown));
    rows.push_back(L"Trades: " + std::to_wstring(s.trades.size()));
    for (size_t i = 0; i < rows.size(); ++i) {
        const int col = static_cast<int>(i % 6);
        const int row = static_cast<int>(i / 6);
        text(dc, x0 + col * 140, y0 + row * 26, rows[i], i == 2 ? priceColor : CLR_TEXT);
    }
}

void drawTabsBar(HDC dc)
{
    RECT tabs = g_app->rects.tabs;
    HBRUSH orange = CreateSolidBrush(CLR_ORANGE);
    FillRect(dc, &tabs, orange);
    DeleteObject(orange);
    text(dc, tabs.left + 14, tabs.top + 10, L"Period", RGB(255, 255, 255));
    text(dc, tabs.left + 214, tabs.top + 10, L"Speed", RGB(255, 255, 255));
    text(dc, tabs.left + 404, tabs.top + 10, L"Indicator", RGB(255, 255, 255));
    text(dc, tabs.left + 620, tabs.top + 10, L"Main indicators: MA5  MA10  MA20  MA60", RGB(255, 255, 255));
}

void drawSidePanel(HDC dc)
{
    RECT side = g_app->rects.side;
    HBRUSH bg = CreateSolidBrush(RGB(246, 246, 246));
    FillRect(dc, &side, bg);
    DeleteObject(bg);

    int y = side.top + 16;
    const int sy = side.bottom - 276;
    const int fieldX = side.left + 18;
    RECT settingsBg{ side.left + 4, sy - 8, side.right - 4, side.bottom - 8 };
    HBRUSH settingsBrush = CreateSolidBrush(RGB(246, 246, 246));
    FillRect(dc, &settingsBg, settingsBrush);
    DeleteObject(settingsBrush);

    text(dc, side.left + 14, y, L"Concurrency Monitor", CLR_TEXT);
    y += 24;
    const auto shortId = [](const std::wstring& id) {
        if (id.size() <= 8) {
            return id;
        }
        return id.substr(id.size() - 8);
    };
    const auto stateText = [](bool active) { return active ? L"RUN" : L"IDLE"; };
    const auto stateColor = [](bool active) { return active ? CLR_RED : CLR_MUTED; };
    const auto& c = g_app->snapshot.concurrency;
    std::wstringstream line;
    line << L"Market   " << stateText(c.marketActive) << L" #" << shortId(c.marketThreadId)
         << L" bars " << c.marketEvents;
    text(dc, side.left + 22, y, line.str(), stateColor(c.marketActive));
    y += 20;
    line.str(L"");
    line.clear();
    line << L"Strategy " << stateText(c.strategyActive) << L" #" << shortId(c.strategyThreadId)
         << L" sig " << c.strategySignals;
    text(dc, side.left + 22, y, line.str(), stateColor(c.strategyActive));
    y += 20;
    line.str(L"");
    line.clear();
    line << L"Matching " << stateText(c.matchingActive) << L" #" << shortId(c.matchingThreadId)
         << L" ord " << c.matchedOrders;
    text(dc, side.left + 22, y, line.str(), stateColor(c.matchingActive));
    y += 20;
    line.str(L"");
    line.clear();
    line << L"Optimize " << stateText(c.optimizationActive) << L" async " << c.optimizationTasks;
    text(dc, side.left + 22, y, line.str(), stateColor(c.optimizationActive));
    y += 20;
    line.str(L"");
    line.clear();
    line << L"UI PostMessage " << c.uiPostMessages;
    text(dc, side.left + 22, y, line.str(), CLR_MUTED);
    y += 20;
    text(dc, side.left + 22, y, L"Parallel MA: std::execution::par", CLR_MUTED);

    y += 28;
    text(dc, side.left + 14, y, L"User Load Monitor", CLR_TEXT);
    y += 24;
    line.str(L"");
    line.clear();
    line << L"Active users " << c.activeUsers << L"  requests " << c.userRequests;
    text(dc, side.left + 22, y, line.str(), c.userLoadActive ? CLR_RED : CLR_MUTED);
    y += 22;
    int userRows = 0;
    for (const auto& user : c.users) {
        if (userRows >= 5 || y + 20 >= sy - 12) {
            break;
        }
        line.str(L"");
        line.clear();
        line << L"U" << user.id << L" "
             << (user.active ? L"RUN" : L"IDLE")
             << L" eq " << std::fixed << std::setprecision(0) << user.equity
             << L"  " << user.latencyMs << L"ms";
        text(dc, side.left + 22, y, line.str(), user.active ? CLR_RED : CLR_MUTED);
        y += 20;
        ++userRows;
    }
    if (c.users.size() > static_cast<size_t>(userRows) && y + 18 < sy) {
        text(dc, side.left + 22, y, L"... more simulated users", CLR_MUTED);
    }

    text(dc, fieldX, sy + 8, L"Backtest Settings", CLR_TEXT);
    text(dc, fieldX, sy + 30, L"Strategy", CLR_MUTED);
    text(dc, fieldX, sy + 94, L"Initial Cash", CLR_MUTED);
    text(dc, fieldX, sy + 158, L"Short MA", CLR_MUTED);
    text(dc, fieldX + 136, sy + 158, L"Long MA", CLR_MUTED);
}

void drawDashboard(HWND window, HDC dc)
{
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, g_app->bgBrush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_TEXT);
    drawQuoteBar(dc);
    drawTabsBar(dc);
    drawKLine(dc, g_app->rects.chart, g_app->snapshot.bars, g_app->visibleBars);
    drawVolumePanel(dc, g_app->rects.volume, g_app->snapshot.bars, g_app->visibleBars);
    drawIndicatorPanel(dc, g_app->rects.indicator, g_app->snapshot.bars, g_app->visibleBars);
    drawSidePanel(dc);

    text(dc, g_app->rects.indicator.left, g_app->rects.indicator.bottom + 6, g_app->chartHint, CLR_MUTED);
}

void drawLabels(HDC dc)
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_MUTED);
    TextOutW(dc, g_app->rects.bottom.left, g_app->rects.bottom.top, L"Orders", 6);
    TextOutW(dc, g_app->rects.bottom.left + ((g_app->rects.bottom.right - g_app->rects.bottom.left - 20) / 3) + 10, g_app->rects.bottom.top, L"Trades", 6);
    TextOutW(dc, g_app->rects.bottom.left + (((g_app->rects.bottom.right - g_app->rects.bottom.left - 20) / 3) + 10) * 2, g_app->rects.bottom.top, L"Logs", 4);
}

INT_PTR CALLBACK About(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    if (message == WM_INITDIALOG) {
        return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}

} // namespace

ATOM RegisterMainWindowClass(HINSTANCE instance)
{
    g_instance = instance;
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = MainWindowProc;
    wcex.hInstance = instance;
    wcex.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_STOCKSYSTEM));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_STOCKSYSTEM);
    wcex.lpszClassName = kWindowClass;
    wcex.hIconSm = LoadIcon(instance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

BOOL InitMainWindow(HINSTANCE instance, int showCommand)
{
    g_instance = instance;
    HWND window = CreateWindowW(
        kWindowClass,
        L"Low-Latency Quant Backtest and Matching Simulator",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        0,
        1180,
        760,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!window) {
        return FALSE;
    }
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    return TRUE;
}

LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_app = std::make_unique<AppState>();
        createControls(window);
        g_app->engine.reset();
        refreshLists(window);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout(window);
        InvalidateRect(window, nullptr, TRUE);
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_START:
            startBacktest(window);
            break;
        case IDC_BTN_PAUSE:
            g_app->engine.pauseOrResume();
            refreshLists(window);
            break;
        case IDC_BTN_RESET:
            g_app->engine.reset();
            refreshLists(window);
            break;
        case IDC_BTN_OPTIMIZE:
            g_app->engine.optimize(window);
            break;
        case IDC_COMBO_PERIOD:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_app->periodFactor = selectedPeriodFactor();
                refreshLists(window);
            }
            break;
        case IDC_COMBO_SPEED:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_app->engine.setReplayDelay(selectedReplayDelay());
                refreshLists(window);
            }
            break;
        case IDC_COMBO_INDICATOR:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_app->indicatorMode = comboSelection(g_app->hIndicator, 1);
                refreshLists(window);
            }
            break;
        case IDM_ABOUT:
            DialogBox(g_instance, MAKEINTRESOURCE(IDD_ABOUTBOX), window, About);
            break;
        case IDM_EXIT:
            DestroyWindow(window);
            break;
        default:
            return DefWindowProc(window, message, wParam, lParam);
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, CLR_PANEL_2);
        SetTextColor(dc, CLR_TEXT);
        return reinterpret_cast<LRESULT>(g_app ? g_app->editBrush : GetStockObject(BLACK_BRUSH));
    }
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) {
            if (!g_app->snapshot.running) {
                startBacktest(window);
                break;
            }
            g_app->engine.pauseOrResume();
        } else if (wParam == 'R') {
            g_app->engine.reset();
        } else if (wParam == VK_ESCAPE) {
            g_app->engine.stop();
        } else if (wParam == VK_F1) {
            MessageBoxW(window,
                L"Space: start or pause backtest\nR: reset\nEsc: stop\nMouse wheel: zoom K-line bars\nLeft click chart: inspect OHLC price",
                L"Help", MB_OK | MB_ICONINFORMATION);
        }
        refreshLists(window);
        break;
    case WM_LBUTTONDOWN:
        if (g_app) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            RECT chartInner = g_app->rects.chart;
            if (PtInRect(&chartInner, POINT{ x, y }) && !g_app->snapshot.bars.empty()) {
                const auto bars = aggregateBars(g_app->snapshot.bars, g_app->periodFactor);
                const size_t count = std::min(static_cast<size_t>(std::max(20, g_app->visibleBars)), bars.size());
                if (count == 0) {
                    break;
                }
                const int chartWidth = std::max(1, static_cast<int>(chartInner.right - chartInner.left));
                const int relativeX = std::clamp(static_cast<int>(x - chartInner.left), 0, chartWidth);
                const size_t index = std::min(count - 1, static_cast<size_t>(static_cast<double>(relativeX) / chartWidth * count));
                const auto& bar = *(bars.end() - static_cast<std::ptrdiff_t>(count) + static_cast<std::ptrdiff_t>(index));
                g_app->selectedBar = static_cast<int>(index);
                std::wstringstream ss;
                ss << L"Bar " << bar.timestamp
                   << L"  O " << money(bar.open)
                   << L" H " << money(bar.high)
                   << L" L " << money(bar.low)
                   << L" C " << money(bar.close)
                   << L" Vol " << bar.volume;
                g_app->chartHint = ss.str();
                SetWindowTextW(g_app->hStatus, ss.str().c_str());
                InvalidateRect(window, &g_app->rects.chart, FALSE);
            }
        }
        break;
    case WM_MOUSEWHEEL:
        if (g_app) {
            const bool zoomIn = GET_WHEEL_DELTA_WPARAM(wParam) > 0;
            g_app->visibleBars = std::clamp(g_app->visibleBars + (zoomIn ? -15 : 15), 30, 240);
            std::wstringstream ss;
            ss << L"K-line zoom: showing last " << g_app->visibleBars << L" bars";
            g_app->chartHint = ss.str();
            SetWindowTextW(g_app->hStatus, ss.str().c_str());
            InvalidateRect(window, &g_app->rects.chart, FALSE);
        }
        break;
    case WM_MOUSEMOVE:
        if (g_app) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (PtInRect(&g_app->rects.chart, POINT{ x, y })) {
                const auto bars = aggregateBars(g_app->snapshot.bars, g_app->periodFactor);
                std::wstring hoverPrice = g_app->snapshot.lastPrice > 0.0 ? money(g_app->snapshot.lastPrice) : L"--";
                if (!bars.empty()) {
                    const size_t count = std::min(static_cast<size_t>(std::max(20, g_app->visibleBars)), bars.size());
                    const int chartWidth = std::max(1, static_cast<int>(g_app->rects.chart.right - g_app->rects.chart.left));
                    const int relativeX = std::clamp(static_cast<int>(x - g_app->rects.chart.left), 0, chartWidth);
                    const size_t index = std::min(count - 1, static_cast<size_t>(static_cast<double>(relativeX) / chartWidth * count));
                    const auto& bar = *(bars.end() - static_cast<std::ptrdiff_t>(count) + static_cast<std::ptrdiff_t>(index));
                    hoverPrice = money(bar.close);
                }
                std::wstringstream ss;
                ss << L"Mouse chart position x=" << x << L", y=" << y
                   << L" | Hover price " << hoverPrice
                   << L" | Last price " << money(g_app->snapshot.lastPrice);
                SetWindowTextW(g_app->hStatus, ss.str().c_str());
            }
        }
        break;
    case WM_APP_ENGINE_UPDATE:
        refreshLists(window);
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC memDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);
        drawDashboard(window, memDc);
        drawLabels(memDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memDc);
        EndPaint(window, &ps);
        break;
    }
    case WM_DESTROY:
        if (g_app) {
            g_app->engine.stop();
            g_app.reset();
        }
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(window, message, wParam, lParam);
    }
    return 0;
}
