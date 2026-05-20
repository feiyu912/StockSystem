#include "MainWindow.h"

#include "BacktestEngine.h"
#include "Indicators.h"
#include "Resource.h"

#include <algorithm>
#include <commctrl.h>
#include <cstdlib>
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

constexpr COLORREF CLR_BG = RGB(10, 15, 25);
constexpr COLORREF CLR_PANEL = RGB(15, 23, 42);
constexpr COLORREF CLR_PANEL_2 = RGB(20, 31, 51);
constexpr COLORREF CLR_BORDER = RGB(51, 65, 85);
constexpr COLORREF CLR_TEXT = RGB(226, 232, 240);
constexpr COLORREF CLR_MUTED = RGB(148, 163, 184);
constexpr COLORREF CLR_GRID = RGB(30, 41, 59);
constexpr COLORREF CLR_RED = RGB(248, 113, 113);
constexpr COLORREF CLR_GREEN = RGB(52, 211, 153);
constexpr COLORREF CLR_BLUE = RGB(96, 165, 250);
constexpr COLORREF CLR_YELLOW = RGB(250, 204, 21);

HINSTANCE g_instance = nullptr;
const wchar_t* kWindowClass = L"StockSystemWin32Class";

struct Rects {
    RECT chart{};
    RECT side{};
    RECT bottom{};
};

struct AppState {
    HWND hStart = nullptr;
    HWND hPause = nullptr;
    HWND hReset = nullptr;
    HWND hOptimize = nullptr;
    HWND hStrategy = nullptr;
    HWND hOrders = nullptr;
    HWND hTrades = nullptr;
    HWND hLogs = nullptr;
    HWND hCash = nullptr;
    HWND hShort = nullptr;
    HWND hLong = nullptr;
    HWND hStatus = nullptr;
    Rects rects;
    int visibleBars = 110;
    std::wstring chartHint = L"Synthetic in-memory market data. Click a bar for OHLC; mouse wheel zooms.";
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
    status << L"Price " << money(g_app->snapshot.lastPrice)
           << L" | Cash " << money(g_app->snapshot.account.cash)
           << L" | Position " << g_app->snapshot.account.position
           << L" | Equity " << money(g_app->snapshot.account.equity)
           << L" | MaxDD " << percent(g_app->snapshot.account.maxDrawdown);
    SetWindowTextW(g_app->hStatus, status.str().c_str());
    InvalidateRect(window, &g_app->rects.chart, FALSE);
}

void startBacktest(HWND window)
{
    const double cash = readDouble(g_app->hCash, 100000.0);
    const int shortW = std::max(1, readInt(g_app->hShort, 5));
    const int longW = std::max(shortW + 1, readInt(g_app->hLong, 30));
    g_app->engine.configure(cash, shortW, longW);
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
    const int top = 14;
    const int sideWidth = 260;
    const int bottomHeight = 190;
    const int gap = 10;

    g_app->rects.chart = { 14, top + 42, width - sideWidth - gap * 2, height - bottomHeight - gap };
    g_app->rects.side = { width - sideWidth - gap, top, width - gap, height - bottomHeight - gap };
    g_app->rects.bottom = { 14, height - bottomHeight, width - 14, height - 32 };

    MoveWindow(g_app->hStart, 14, top, 72, 28, TRUE);
    MoveWindow(g_app->hPause, 92, top, 72, 28, TRUE);
    MoveWindow(g_app->hReset, 170, top, 72, 28, TRUE);
    MoveWindow(g_app->hOptimize, 248, top, 104, 28, TRUE);

    const int sx = g_app->rects.side.left;
    const int sy = g_app->rects.side.top;
    MoveWindow(g_app->hStrategy, sx, sy + 20, sideWidth - 20, 26, TRUE);
    MoveWindow(g_app->hCash, sx, sy + 78, sideWidth - 20, 24, TRUE);
    MoveWindow(g_app->hShort, sx, sy + 136, 110, 24, TRUE);
    MoveWindow(g_app->hLong, sx + 128, sy + 136, 110, 24, TRUE);

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

    for (HWND child : { g_app->hStart, g_app->hPause, g_app->hReset, g_app->hOptimize, g_app->hStrategy,
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
    POINT first = pointAt(0);
    MoveToEx(dc, first.x, first.y, nullptr);
    for (size_t i = 1; i < values.size(); ++i) {
        POINT p = pointAt(i);
        LineTo(dc, p.x, p.y);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void drawKLine(HDC dc, const RECT& rect, const std::vector<MarketBar>& allBars, int visibleBars)
{
    if (allBars.empty()) {
        SetTextColor(dc, CLR_MUTED);
        DrawTextW(dc, L"No market data yet. Press Start or Space.", -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const size_t count = std::min(static_cast<size_t>(std::max(20, visibleBars)), allBars.size());
    const auto first = allBars.end() - static_cast<std::ptrdiff_t>(count);
    std::vector<MarketBar> bars(first, allBars.end());
    std::vector<double> closes;
    closes.reserve(bars.size());

    double minPrice = bars.front().low;
    double maxPrice = bars.front().high;
    for (const auto& bar : bars) {
        minPrice = std::min(minPrice, bar.low);
        maxPrice = std::max(maxPrice, bar.high);
        closes.push_back(bar.close);
    }
    const auto ma5 = movingAverageParallel(closes, 5);
    const auto ma20 = movingAverageParallel(closes, 20);

    const double span = std::max(0.0001, maxPrice - minPrice);
    auto yOf = [&](double price) {
        const double ratio = (price - minPrice) / span;
        return rect.bottom - static_cast<int>(ratio * (rect.bottom - rect.top));
    };

    HPEN gridPen = CreatePen(PS_SOLID, 1, CLR_GRID);
    HGDIOBJ oldPen = SelectObject(dc, gridPen);
    for (int i = 1; i < 5; ++i) {
        const int y = rect.top + (rect.bottom - rect.top) * i / 5;
        MoveToEx(dc, rect.left, y, nullptr);
        LineTo(dc, rect.right, y);
    }
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);

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
    drawLineWithScale(dc, rect, ma20, minPrice, maxPrice, CLR_BLUE, 2);
}

void drawDashboard(HWND window, HDC dc)
{
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, g_app->bgBrush);

    RECT chart = g_app->rects.chart;
    FillRect(dc, &chart, g_app->panelBrush);
    HBRUSH border = CreateSolidBrush(CLR_BORDER);
    FrameRect(dc, &chart, border);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_TEXT);
    RECT title = chart;
    title.left += 16;
    title.top += 10;
    DrawTextW(dc, L"TEST.SH  K-Line / MA / Equity", -1, &title, DT_TOP | DT_LEFT | DT_SINGLELINE);

    RECT inner = chart;
    inner.left += 20;
    inner.right -= 16;
    inner.top += 42;
    inner.bottom -= 88;
    drawKLine(dc, inner, g_app->snapshot.bars, g_app->visibleBars);

    RECT equityRect = chart;
    equityRect.left += 20;
    equityRect.right -= 16;
    equityRect.top = inner.bottom + 18;
    equityRect.bottom -= 22;
    HBRUSH equityBrush = CreateSolidBrush(CLR_PANEL_2);
    FillRect(dc, &equityRect, equityBrush);
    DeleteObject(equityBrush);
    FrameRect(dc, &equityRect, border);
    drawSeries(dc, equityRect, g_app->snapshot.equities, CLR_GREEN);

    RECT hint = chart;
    hint.left += 16;
    hint.top = chart.bottom - 20;
    SetTextColor(dc, CLR_MUTED);
    DrawTextW(dc, g_app->chartHint.c_str(), -1, &hint, DT_LEFT | DT_SINGLELINE);
    DeleteObject(border);
}

void drawLabels(HDC dc)
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_MUTED);
    RECT r = g_app->rects.side;
    TextOutW(dc, r.left, r.top, L"Strategy", 8);
    TextOutW(dc, r.left, r.top + 58, L"Initial Cash", 12);
    TextOutW(dc, r.left, r.top + 116, L"Short MA", 8);
    TextOutW(dc, r.left + 128, r.top + 116, L"Long MA", 7);
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
            chartInner.left += 20;
            chartInner.right -= 16;
            chartInner.top += 42;
            chartInner.bottom -= 88;
            if (PtInRect(&chartInner, POINT{ x, y }) && !g_app->snapshot.bars.empty()) {
                const size_t count = std::min(static_cast<size_t>(std::max(20, g_app->visibleBars)), g_app->snapshot.bars.size());
                const int chartWidth = std::max(1, static_cast<int>(chartInner.right - chartInner.left));
                const int relativeX = std::clamp(static_cast<int>(x - chartInner.left), 0, chartWidth);
                const size_t index = std::min(count - 1, static_cast<size_t>(static_cast<double>(relativeX) / chartWidth * count));
                const auto& bar = *(g_app->snapshot.bars.end() - static_cast<std::ptrdiff_t>(count) + static_cast<std::ptrdiff_t>(index));
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
                std::wstringstream ss;
                ss << L"Mouse chart position x=" << x << L", y=" << y
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
