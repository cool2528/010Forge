#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "ui_text.h"

// GDI-only tracker skin. Palette and drawings are shared with the real window;
// documentation screenshots are rendered from that same window, not mockups.
// Meter bars are a static tracker motif, not an audio-analysis visualization.
namespace forge::tracker {
inline constexpr COLORREF Background = RGB(10, 13, 18);
inline constexpr COLORREF Panel = RGB(18, 24, 31);
inline constexpr COLORREF Input = RGB(7, 11, 16);
inline constexpr COLORREF Button = RGB(27, 37, 45);
inline constexpr COLORREF Border = RGB(47, 69, 77);
inline constexpr COLORREF Green = RGB(157, 255, 107);
inline constexpr COLORREF Cyan = RGB(80, 216, 213);
inline constexpr COLORREF Amber = RGB(244, 189, 88);
inline constexpr COLORREF Text = RGB(205, 222, 224);
inline constexpr COLORREF Muted = RGB(115, 143, 151);
inline HBRUSH pageBrush{}, panelBrush{}, inputBrush{};
inline HFONT titleFont{}, smallFont{};
inline UINT dpi = 96;
inline int S(int v) { return MulDiv(v, static_cast<int>(dpi), 96); }
inline RECT Box(int x, int y, int w, int h) { return {S(x), S(y), S(x+w), S(y+h)}; }
inline void Fill(HDC dc, RECT r, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush); DeleteObject(brush);
}
inline void Outline(HDC dc, RECT r, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &r, brush); DeleteObject(brush);
}
inline void Line(HDC dc, int x, int y, int x2, int y2, COLORREF color) {
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, S(x), S(y), nullptr); LineTo(dc, S(x2), S(y2));
    SelectObject(dc, old); DeleteObject(pen);
}
inline void Label(HDC dc, const wchar_t* text, RECT r, COLORREF color, HFONT font,
                  UINT format=DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX) {
    const int saved = SaveDC(dc);
    SelectObject(dc, font); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &r, format); RestoreDC(dc, saved);
}
inline void Initialize(UINT scale) {
    dpi=scale;
    pageBrush=CreateSolidBrush(Background); panelBrush=CreateSolidBrush(Panel); inputBrush=CreateSolidBrush(Input);
    titleFont=CreateFontW(-S(32),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,FIXED_PITCH,L"Consolas");
    smallFont=CreateFontW(-S(11),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,FIXED_PITCH,L"Consolas");
}
inline void Destroy() {
    DeleteObject(pageBrush); DeleteObject(panelBrush); DeleteObject(inputBrush);
    DeleteObject(titleFont); DeleteObject(smallFont);
}
inline void Paint(HWND window, HFONT font, bool music, const wchar_t* version) {
    PAINTSTRUCT ps{}; const HDC dc=BeginPaint(window,&ps);
    RECT client{}; GetClientRect(window,&client); FillRect(dc,&client,pageBrush);
    for(int y=0;y<client.bottom;y+=S(4)) {
        RECT row{0,y,client.right,y+1}; Fill(dc,row,RGB(12,17,22));
    }
    for(const RECT r : {Box(16,14,800,94),Box(16,120,800,116),Box(16,356,800,236)}) {
        Fill(dc,r,Panel); Outline(dc,r,Border);
    }
    Line(dc,16,14,816,14,Green);
    Label(dc,L"010FORGE",Box(31,23,258,38),Green,titleFont);
    const std::wstring v=std::wstring(L"v")+version+L"  //  TRACKER EDITION";
    Label(dc,v.c_str(),Box(291,31,355,20),Cyan,smallFont);
    Label(dc,L"TERMINAL PULSE   /   168 BPM   /   STEREO",Box(32,84,596,16),Muted,smallFont);
    const int heights[]={15,28,41,29,49,37,23,32};
    for(int i=0;i<8;++i) {
        for(int vbar=0;vbar<(music?heights[i]:5);vbar+=6) {
            Fill(dc,Box(670+i*16,87-vbar,11,4),vbar>35?Amber:Green);
        }
    }
    Label(dc,L"01 / CONFIG",Box(31,112,150,16),Cyan,smallFont);
    Label(dc,L"02 / OPERATIONS",Box(31,241,220,14),Cyan,smallFont);
    Label(dc,L"03 / EVENT CONSOLE",Box(31,348,242,16),Cyan,smallFont);
    Label(dc,L"CHANNELS  00:01:02:03:04:05:06:07",Box(459,362,340,18),Muted,smallFont);
    Line(dc,32,608,800,608,Border);
    Label(dc,music?L"[ PLAY ]  TERMINAL_PULSE_168.WAV":L"[ STOP ]  MODULE MUTED",
          Box(32,616,535,20),music?Green:Muted,smallFont);
    EndPaint(window,&ps);
    (void)font;
}
inline bool DrawControl(const DRAWITEMSTRUCT* item, HFONT font, bool music) {
    if(!item) return false;
    if(item->CtlType==ODT_BUTTON) {
        const int saved=SaveDC(item->hDC);
        const bool pushed=(item->itemState&ODS_SELECTED)!=0;
        const bool focus=(item->itemState&ODS_FOCUS)!=0;
        const bool disabled=(item->itemState&ODS_DISABLED)!=0;
        const bool danger=item->CtlID==ui::Restore;
        const COLORREF accent=danger?Amber:Cyan;
        RECT r=item->rcItem;
        Fill(item->hDC,r,pushed?RGB(31,67,64):Button);
        Outline(item->hDC,r,focus?Green:Border);
        RECT stripe=r; stripe.right=stripe.left+S(3);
        Fill(item->hDC,stripe,disabled?Muted:accent);
        const int length=GetWindowTextLengthW(item->hwndItem);
        std::vector<wchar_t> text(static_cast<size_t>(length)+1);
        GetWindowTextW(item->hwndItem,text.data(),length+1);
        std::wstring label=text.data();
        if(item->CtlID==ui::Music) label=(music?L"[x] ":L"[ ] ")+label;
        if(pushed) OffsetRect(&r,S(1),S(1));
        InflateRect(&r,-S(5),-S(2));
        Label(item->hDC,label.c_str(),r,disabled?Muted:(danger?Amber:Text),font,
              DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        if(focus) { InflateRect(&r,-S(2),-S(2)); SetTextColor(item->hDC,Muted); DrawFocusRect(item->hDC,&r); }
        RestoreDC(item->hDC,saved); return true;
    }
    if(item->CtlType==ODT_LISTBOX) {
        if(item->itemID==static_cast<UINT>(-1)) return true;
        const bool selected=(item->itemState&ODS_SELECTED)!=0;
        Fill(item->hDC,item->rcItem,selected?RGB(24,49,53):Input);
        const LRESULT n=SendMessageW(item->hwndItem,LB_GETTEXTLEN,item->itemID,0);
        if(n==LB_ERR) return true;
        std::vector<wchar_t> text(static_cast<size_t>(n)+1);
        SendMessageW(item->hwndItem,LB_GETTEXT,item->itemID,reinterpret_cast<LPARAM>(text.data()));
        wchar_t row[16]{}; swprintf_s(row,L"%02X",item->itemID);
        RECT nr=item->rcItem; nr.left+=S(7); nr.right=nr.left+S(27);
        Label(item->hDC,row,nr,Muted,smallFont);
        RECT tr=item->rcItem; tr.left+=S(42);
        const std::wstring value=text.data();
        const COLORREF color=value.find(L"[ERROR]")!=std::wstring::npos||value.find(L"[错误]")!=std::wstring::npos
                           ? Amber : Green;
        Label(item->hDC,text.data(),tr,color,font);
        return true;
    }
    return false;
}
} // namespace forge::tracker
