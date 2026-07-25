# -*- coding: utf-8 -*-
"""
APM Agent & Console 포트폴리오 PPT 생성 스크립트.
참고 파일(김서현_MonsterGround_포트폴리오_v4.pptx)에서 추출한 디자인 시스템을 그대로 재사용:
- 라이트 테마, 좌측 액센트 바, 블루(#2563EB) 주 색상
- 문제=레드(#DC2626)/판단=블루/해결=그린(#16A34A) 3색 카드
- 코드 블록은 다크(#1A1A2E) 배경 + Courier New
"""
from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn

# ── 색상 팔레트 (참고 PPT에서 추출) ──
ACCENT      = RGBColor(0x25, 0x63, 0xEB)  # blue-600
DARK        = RGBColor(0x0F, 0x17, 0x2A)  # slate-900 (제목)
GRAY_BODY   = RGBColor(0x6B, 0x72, 0x80)  # gray-500 (서브텍스트/라벨)
GRAY_TEXT   = RGBColor(0x37, 0x41, 0x51)  # gray-700 (본문)
GRAY_MUTED  = RGBColor(0x9C, 0xA3, 0xAF)  # gray-400 (페이지 번호 등)
BORDER      = RGBColor(0xE2, 0xE8, 0xF0)  # slate-200
BORDER2     = RGBColor(0xCB, 0xD5, 0xE1)  # slate-300
CARD_BG     = RGBColor(0xF8, 0xFA, 0xFC)  # slate-50
CALLOUT_BG  = RGBColor(0xEF, 0xF6, 0xFF)  # blue-50
RED         = RGBColor(0xDC, 0x26, 0x26)
RED_BG      = RGBColor(0xFE, 0xF2, 0xF2)
GREEN       = RGBColor(0x16, 0xA3, 0x4A)
GREEN_BG    = RGBColor(0xF0, 0xFD, 0xF4)
AMBER       = RGBColor(0xB4, 0x53, 0x09)
AMBER_BG    = RGBColor(0xFF, 0xFB, 0xEB)
CODE_BG     = RGBColor(0x1A, 0x1A, 0x2E)
CODE_TEXT   = RGBColor(0xCD, 0xD9, 0xE5)
CODE_CMT    = RGBColor(0x8B, 0x94, 0x9E)
CODE_RED    = RGBColor(0xFF, 0x6B, 0x6B)
CODE_GREEN  = RGBColor(0x7E, 0xE7, 0x87)
WHITE       = RGBColor(0xFF, 0xFF, 0xFF)

FONT = "Calibri"
FONT_CODE = "Courier New"

SLIDE_W = Inches(10)
SLIDE_H = Inches(5.625)
TOTAL_PAGES = 12

prs = Presentation()
prs.slide_width = SLIDE_W
prs.slide_height = SLIDE_H
blank_layout = prs.slide_layouts[6]


def add_slide():
    return prs.slides.add_slide(blank_layout)


def set_fill(shape, color):
    shape.fill.solid()
    shape.fill.fore_color.rgb = color
    shape.line.fill.background()


def set_border(shape, color, weight_pt=0.75, fill=None):
    if fill is None:
        shape.fill.background()
    else:
        shape.fill.solid()
        shape.fill.fore_color.rgb = fill
    shape.line.color.rgb = color
    shape.line.width = Pt(weight_pt)


def no_shadow(shape):
    shape.shadow.inherit = False


def rect(slide, x, y, w, h, shape_type=MSO_SHAPE.RECTANGLE):
    sp = slide.shapes.add_shape(shape_type, x, y, w, h)
    no_shadow(sp)
    return sp


def textbox(slide, x, y, w, h, valign=MSO_ANCHOR.TOP):
    tb = slide.shapes.add_textbox(x, y, w, h)
    tf = tb.text_frame
    tf.word_wrap = True
    tf.vertical_anchor = valign
    tf.margin_left = 0
    tf.margin_right = 0
    tf.margin_top = 0
    tf.margin_bottom = 0
    return tb, tf


def set_run(run, text, size, color, bold=False, font=FONT, italic=False):
    run.text = text
    run.font.size = Pt(size)
    run.font.color.rgb = color
    run.font.bold = bold
    run.font.italic = italic
    run.font.name = font
    # 한글 폰트도 명시(문서 전체 동일 폰트로 렌더링되게)
    rPr = run._r.get_or_add_rPr()
    ea = rPr.find(qn('a:ea'))
    if ea is None:
        ea = rPr.makeelement(qn('a:ea'), {})
        rPr.append(ea)
    ea.set('typeface', font)


def simple_text(slide, x, y, w, h, text, size, color, bold=False, align=PP_ALIGN.LEFT, font=FONT, valign=MSO_ANCHOR.TOP):
    tb, tf = textbox(slide, x, y, w, h, valign=valign)
    p = tf.paragraphs[0]
    p.alignment = align
    r = p.add_run()
    set_run(r, text, size, color, bold=bold, font=font)
    return tb


def multi_run_text(slide, x, y, w, h, runs, size, align=PP_ALIGN.LEFT, valign=MSO_ANCHOR.TOP, line_spacing=None):
    """runs: [(text, color, bold), ...] 한 문단 안에서 색/굵기가 섞인 텍스트"""
    tb, tf = textbox(slide, x, y, w, h, valign=valign)
    p = tf.paragraphs[0]
    p.alignment = align
    if line_spacing:
        p.line_spacing = line_spacing
    for text, color, bold in runs:
        r = p.add_run()
        set_run(r, text, size, color, bold=bold)
    return tb


def left_bar(slide, width_in=0.07):
    bar = rect(slide, 0, 0, Inches(width_in), SLIDE_H)
    set_fill(bar, ACCENT)


def page_header(slide, label, title, subtitle=None, page=1):
    left_bar(slide)
    simple_text(slide, Inches(0.25), Inches(0.38), Inches(9.5), Inches(0.5), title, 23, DARK, bold=True)
    # 좌상단의 별도 "// 라벨" 주석 줄은 없애고, 라벨 내용을 부제목 문장 안에 자연스럽게 포함시킴
    clean_label = label.lstrip("/ ").strip()
    merged_subtitle = f"{clean_label} · {subtitle}" if subtitle else clean_label
    simple_text(slide, Inches(0.25), Inches(0.87), Inches(9.5), Inches(0.22), merged_subtitle, 9, GRAY_BODY)
    divider = rect(slide, Inches(0.25), Inches(1.12), Inches(9.5), Pt(0.75))
    set_fill(divider, BORDER)
    # page number
    simple_text(slide, Inches(8.70), Inches(5.35), Inches(1.10), Inches(0.20),
                f"{page} / {TOTAL_PAGES}", 7.5, GRAY_MUTED, align=PP_ALIGN.RIGHT)


def decision_card(slide, x, y, w, h, num, title, problem, judgment, solution):
    card = rect(slide, x, y, w, h, MSO_SHAPE.ROUNDED_RECTANGLE)
    card.adjustments[0] = 0.04
    set_border(card, BORDER, 0.75, fill=WHITE)

    simple_text(slide, x + Inches(0.15), y + Inches(0.10), w - Inches(0.3), Inches(0.18),
                f"// {num}", 7.5, GRAY_MUTED, font=FONT_CODE)
    simple_text(slide, x + Inches(0.15), y + Inches(0.26), w - Inches(0.3), Inches(0.36),
                title, 10.5, DARK, bold=True)

    gap = Inches(0.04)
    header_h = Inches(0.65)
    available = h - header_h - gap * 2
    row_h = int(available / 3)
    labels = [("문제", RED, RED_BG, problem), ("판단", ACCENT, CALLOUT_BG, judgment), ("해결", GREEN, GREEN_BG, solution)]
    ry = y + header_h
    for label, color, bg, text in labels:
        box = rect(slide, x + Inches(0.12), ry, w - Inches(0.24), row_h, MSO_SHAPE.ROUNDED_RECTANGLE)
        box.adjustments[0] = 0.08
        set_border(box, color, 0.5, fill=bg)
        simple_text(slide, x + Inches(0.20), ry + Inches(0.02), Inches(0.5), Inches(0.20), label, 7, color, bold=True)
        simple_text(slide, x + Inches(0.62), ry + Inches(0.01), w - Inches(0.78), row_h - Inches(0.04),
                    text, 7.5, GRAY_TEXT)
        ry += row_h + gap


def code_block(slide, x, y, w, h, lines):
    """lines: [[(text, color), (text, color), ...], ...] 줄 단위 run 리스트"""
    box = rect(slide, x, y, w, h, MSO_SHAPE.ROUNDED_RECTANGLE)
    box.adjustments[0] = 0.03
    set_fill(box, CODE_BG)
    tb, tf = textbox(slide, x + Inches(0.15), y + Inches(0.10), w - Inches(0.3), h - Inches(0.2))
    first = True
    for line_runs in lines:
        p = tf.paragraphs[0] if first else tf.add_paragraph()
        first = False
        p.line_spacing = 1.15
        for text, color in line_runs:
            r = p.add_run()
            set_run(r, text, 8, color, font=FONT_CODE)


def bug_panel(slide, x, y, w, h, verdict, header_text, header_color, bg, code_lines, note_runs=None):
    panel = rect(slide, x, y, w, h, MSO_SHAPE.ROUNDED_RECTANGLE)
    panel.adjustments[0] = 0.03
    set_fill(panel, bg)
    simple_text(slide, x + Inches(0.15), y + Inches(0.06), w - Inches(0.3), Inches(0.24),
                f"{verdict}  {header_text}", 9.5, header_color, bold=False)
    code_block(slide, x + Inches(0.03), y + Inches(0.34), w - Inches(0.06), h - Inches(0.85), code_lines)
    if note_runs:
        multi_run_text(slide, x + Inches(0.15), y + h - Inches(0.46), w - Inches(0.3), Inches(0.42), note_runs, 8)


def tag_pill(slide, x, y, text):
    w = Inches(0.14 + 0.072 * len(text))
    h = Inches(0.24)
    pill = rect(slide, x, y, w, h, MSO_SHAPE.ROUNDED_RECTANGLE)
    pill.adjustments[0] = 0.5
    set_border(pill, BORDER, 0.5, fill=CARD_BG)
    simple_text(slide, x, y + Inches(0.03), w, Inches(0.18), text, 7, GRAY_BODY, align=PP_ALIGN.CENTER, font=FONT_CODE)
    return x + w + Inches(0.10)


def stat(slide, x, y, value, label):
    simple_text(slide, x, y, Inches(1.30), Inches(0.48), value, 30, ACCENT, bold=True)
    simple_text(slide, x, y + Inches(0.46), Inches(1.30), Inches(0.20), label, 8, GRAY_BODY)


# =========================================================
# Slide 1 — Hero
# =========================================================
s = add_slide()
left_bar(s, 0.18)

award = rect(s, Inches(0.35), Inches(0.28), Inches(4.55), Inches(0.32), MSO_SHAPE.ROUNDED_RECTANGLE)
award.adjustments[0] = 0.5
set_border(award, AMBER, 0.75, fill=AMBER_BG)
simple_text(s, Inches(0.35), Inches(0.335), Inches(4.55), Inches(0.24),
            "●  전체 파이프라인 end-to-end 동작 · 실측 검증 완료", 8, AMBER, align=PP_ALIGN.CENTER, font=FONT_CODE)

simple_text(s, Inches(0.35), Inches(0.72), Inches(5.5), Inches(0.60), "APM Agent & Console", 32, DARK, bold=True)
simple_text(s, Inches(0.35), Inches(1.42), Inches(5.5), Inches(0.34),
            "경량 모니터링 에이전트 + 실시간 웹 대시보드", 15, ACCENT)
multi_run_text(s, Inches(0.35), Inches(1.80), Inches(5.6), Inches(0.62),
               [("Standalone Asio 크로스플랫폼 코어 위에 TLS·AES-256-GCM 이중 암호화·메시지 프레이밍을 갖춘 "
                 "수집 에이전트와, .NET 8 플러그인 아키텍처 기반 실시간 대시보드까지 1인 설계·구현.",
                 GRAY_BODY, False)], 10, line_spacing=1.3)

divider = rect(s, Inches(0.35), Inches(2.48), Inches(5.5), Pt(0.75))
set_fill(divider, BORDER)

tx = Inches(0.35)
for t in ["C++20 / Asio", "TLS + AES-256-GCM", "Protobuf", ".NET 8", "SignalR", "Linux + Windows"]:
    tx = tag_pill(s, tx, Inches(2.60), t)

stat(s, Inches(0.35), Inches(3.02), "5", "수집 지표 종류")
stat(s, Inches(1.70), Inches(3.02), "17", "단위 테스트 통과")
stat(s, Inches(3.05), Inches(3.02), "0.017%", "유휴 CPU 사용률")
stat(s, Inches(4.40), Inches(3.02), "13.8MB", "RSS 완전 고정")

callout = rect(s, Inches(0.35), Inches(3.82), Inches(5.5), Inches(0.60), MSO_SHAPE.ROUNDED_RECTANGLE)
callout.adjustments[0] = 0.08
set_border(callout, ACCENT, 0.75, fill=CALLOUT_BG)
multi_run_text(s, Inches(0.50), Inches(3.94), Inches(5.20), Inches(0.40),
               [("IPayloadSealer", ACCENT, True),
                (" 추상화로 ARIA→AES 마이그레이션이 세션/프레이밍 코드 변경 없이 완료됨", ACCENT, False)],
               9.5, line_spacing=1.2)

# 우측 패널 — 파이프라인 미니 다이어그램
panel = rect(s, Inches(6.10), Inches(0.28), Inches(3.60), Inches(4.88), MSO_SHAPE.ROUNDED_RECTANGLE)
panel.adjustments[0] = 0.02
set_border(panel, BORDER2, 0.75, fill=CARD_BG)

simple_text(s, Inches(6.30), Inches(0.48), Inches(3.20), Inches(0.22), "// Pipeline", 8, GRAY_MUTED, font=FONT_CODE)

nodes = [
    ("Agent", "수집 · Protobuf 직렬화 · AES-256-GCM", ACCENT),
    ("Collector", "프레이밍 해제 · 저장(SQLite) · 재전송", RED),
    ("APM_Console", "복호화 · SignalR · Chart.js 대시보드", GREEN),
]
ny = Inches(0.85)
node_h = Inches(1.05)
for i, (name, desc, color) in enumerate(nodes):
    nbox = rect(s, Inches(6.35), ny, Inches(3.10), Inches(0.72), MSO_SHAPE.ROUNDED_RECTANGLE)
    nbox.adjustments[0] = 0.08
    set_border(nbox, color, 1.0, fill=WHITE)
    simple_text(s, Inches(6.50), ny + Inches(0.08), Inches(2.80), Inches(0.26), name, 12, color, bold=True)
    simple_text(s, Inches(6.50), ny + Inches(0.36), Inches(2.80), Inches(0.32), desc, 7.5, GRAY_TEXT)
    if i < len(nodes) - 1:
        arrow = rect(s, Inches(7.75), ny + Inches(0.72), Inches(0.02), Inches(0.20))
        set_fill(arrow, BORDER2)
        simple_text(s, Inches(7.55), ny + Inches(0.86), Inches(0.60), Inches(0.18), "▼", 9, GRAY_MUTED, align=PP_ALIGN.CENTER)
    ny += node_h

simple_text(s, Inches(6.35), Inches(4.55), Inches(3.10), Inches(0.50),
            "3개 프로세스, 구간별 별도 AES 키로 분리된 2개의 암호화 구간", 7.5, GRAY_BODY)

simple_text(s, Inches(8.70), Inches(5.35), Inches(1.10), Inches(0.20), "1 / 12", 7.5, GRAY_MUTED, align=PP_ALIGN.RIGHT)


# =========================================================
# Slide 2 — Architecture
# =========================================================
s = add_slide()
page_header(s, "// Architecture", "전체 시스템 구조", "Agent → Collector(로컬 저장) → APM_Console(웹 대시보드) — 구간별 별도 키의 AES-256-GCM.", page=2)

# 파이프라인 바
stages = [
    ("ResourceCollector", "CPU/메모리/디스크/네트워크/TCP 수집"),
    ("PacketHeader 프레이밍", "Protobuf 직렬화 (apm::Metric)"),
    ("AesGcmPayload", "AES-256-GCM (agent_collector_aes.key)"),
    ("TLS (asio::ssl)", "비동기 I/O 코어 (io_context)"),
    ("PacketHandler", "타입 안전 디스패치 → IMetricStore"),
]
bx = Inches(0.25)
bw = Inches(1.78)
by = Inches(1.35)
for i, (title, desc) in enumerate(stages):
    box = rect(s, bx, by, bw, Inches(1.0), MSO_SHAPE.ROUNDED_RECTANGLE)
    box.adjustments[0] = 0.06
    set_border(box, BORDER, 0.75, fill=WHITE if i % 2 == 0 else CARD_BG)
    simple_text(s, bx + Inches(0.10), by + Inches(0.10), bw - Inches(0.2), Inches(0.42), title, 8.5, DARK, bold=True)
    simple_text(s, bx + Inches(0.10), by + Inches(0.50), bw - Inches(0.2), Inches(0.46), desc, 6.8, GRAY_TEXT)
    if i < len(stages) - 1:
        simple_text(s, bx + bw - Inches(0.02), by + Inches(0.36), Inches(0.28), Inches(0.28), "→", 12, ACCENT, align=PP_ALIGN.CENTER)
    bx += bw + Inches(0.16)

simple_text(s, Inches(0.25), Inches(2.50), Inches(9.5), Inches(0.22),
            "Collector는 저장과 별개로 ResilientSender(재연결 큐잉)를 통해 AES-256-GCM으로 APM_Console에도 전송", 8.5, GRAY_BODY)

# 구간 카드 2개
for i, (label, keyfile, desc) in enumerate([
    ("Agent ↔ Collector", "agent_collector_aes.key",
     "원래 ARIA-256-CBC+HMAC(KISA 표준 경험 목적)이었으나, 구조적 안전성(AEAD)과 단일 검증 경로를 위해 "
     "AES-256-GCM으로 통일. 구현 경험은 ARIA_TO_AES_MIGRATION.md에 보존."),
    ("Collector ↔ Console", "webserver_aes.key",
     ".NET(AesGcm)과의 상호운용성을 위해 처음부터 AES-256-GCM 채택. IPayloadSealer 인터페이스로 "
     "세션은 암호 방식을 모르는 채 구현체만 주입받음."),
]):
    cx = Inches(0.25) + Inches(4.75) * i
    card = rect(s, cx, Inches(2.85), Inches(4.55), Inches(2.05), MSO_SHAPE.ROUNDED_RECTANGLE)
    card.adjustments[0] = 0.04
    set_border(card, BORDER, 0.75, fill=WHITE)
    simple_text(s, cx + Inches(0.18), Inches(3.00), Inches(4.2), Inches(0.20), f"// {label}", 8, GRAY_MUTED, font=FONT_CODE)
    simple_text(s, cx + Inches(0.18), Inches(3.22), Inches(4.2), Inches(0.30), "AES-256-GCM", 13, ACCENT, bold=True)
    simple_text(s, cx + Inches(0.18), Inches(3.54), Inches(4.2), Inches(0.20), f"키: {keyfile}", 8, GRAY_BODY, font=FONT_CODE)
    multi_run_text(s, cx + Inches(0.18), Inches(3.80), Inches(4.2), Inches(1.0), [(desc, GRAY_TEXT, False)], 8, line_spacing=1.25)


# =========================================================
# Slide 3 — Design Decisions 01-03
# =========================================================
s = add_slide()
page_header(s, "// Design Decisions", "설계 의사결정  01 – 03", '"왜 이렇게 만들었는가"', page=3)

cards3 = [
    ("01", "Standalone Asio 채택",
     "기존 서버가 Windows IOCP에 강결합, Linux에서 동작 불가.",
     "Asio가 이미 추상화한 IOCP/epoll을 채택 — 비동기 구조는 기존 설계 유지.",
     "Linux에서 빌드+실행 검증(포트 리스닝, 무크래시)."),
    ("02", "ApmSession 독립 작성",
     "공유 Session에 TLS를 얹으면 TLS 안 쓰는 소비자까지 영향.",
     "TLS 소비자가 하나뿐인 시점의 책임 확장은 과설계(YAGNI).",
     "Session 완전 무수정, ApmSession이 프레이밍·암호화·재연결 전담."),
    ("03", "메시지 프레이밍 + Protobuf 리플렉션",
     "TCP는 메시지 경계가 없어 read 1회=메시지 1개 가정은 설계 공백.",
     "PacketHeader{size,id}로 프레이밍, ID는 descriptor()->index()로 자동 결정.",
     "PacketHandler::Register<T>()가 역직렬화까지 자동 — ID 충돌 구조적으로 불가능."),
]
cx = Inches(0.25)
for num, title, p, j, sol in cards3:
    decision_card(s, cx, Inches(1.18), Inches(3.03), Inches(2.35), num, title, p, j, sol)
    cx += Inches(3.23)


# =========================================================
# Slide 4 — Design Decisions 04-06 + 저장소 비교 표
# =========================================================
s = add_slide()
page_header(s, "// Design Decisions", "설계 의사결정  04 – 06", '"왜 이렇게 만들었는가"', page=4)

cards4 = [
    ("04", "IMetricStore 컴파일 타임 선택",
     "SQLite·TimescaleDB 둘 다 지원하되, 안 쓰는 백엔드 의존성이 남으면 안 됨.",
     "런타임 분기 대신 인터페이스+CMake 옵션(APM_STORAGE_BACKEND)으로 컴파일 타임 고정.",
     "ldd로 libpq 미링크 확인. .NET은 두 프로바이더 다 참조 가능해 런타임 선택 — 언어별 차이."),
    ("05", "PrivilegeDrop 최소 권한",
     "에이전트가 침해당했을 때 과도한 권한이면 피해 범위가 커짐.",
     "포트 바인딩 등 특권 초기화 직후 setuid/setgid로 nobody까지 하향.",
     "gid→보조그룹 제거→uid 순서, 하향 후 재상승 실패까지 방어적 검증."),
    ("06", "도메인별 RCL 플러그인",
     "Apm/Game 도메인을 한 프로젝트에 두면 컨벤션을 잊고 오염될 위험.",
     "공수가 더 들어도 컴파일 타임에 강제되는 도메인 격리를 우선.",
     "RCL+AssemblyLoadContext 로딩. 호스트가 로드한 공유 타입은 재사용해 ALC 타입 충돌 회피."),
]
cx = Inches(0.25)
for num, title, p, j, sol in cards4:
    decision_card(s, cx, Inches(1.18), Inches(3.03), Inches(2.05), num, title, p, j, sol)
    cx += Inches(3.23)

# 하단 비교 표
table_box = rect(s, Inches(0.25), Inches(3.45), Inches(9.5), Inches(1.75), MSO_SHAPE.ROUNDED_RECTANGLE)
table_box.adjustments[0] = 0.03
set_border(table_box, BORDER, 0.75, fill=CARD_BG)
simple_text(s, Inches(0.40), Inches(3.52), Inches(9.2), Inches(0.20), "// SQLite vs TimescaleDB", 7.5, GRAY_BODY, font=FONT_CODE)

headers = ["항목", "SQLite (기본)", "TimescaleDB"]
col_x = [0.40, 2.90, 6.30]
col_w = [2.40, 3.30, 3.30]
simple_text(s, Inches(col_x[0]), Inches(3.75), Inches(col_w[0]), Inches(0.20), headers[0], 7.5, GRAY_BODY, bold=True)
simple_text(s, Inches(col_x[1]), Inches(3.75), Inches(col_w[1]), Inches(0.20), headers[1], 7.5, GRAY_BODY, bold=True)
simple_text(s, Inches(col_x[2]), Inches(3.75), Inches(col_w[2]), Inches(0.20), headers[2], 7.5, GRAY_BODY, bold=True)
line = rect(s, Inches(0.40), Inches(3.96), Inches(9.20), Pt(0.5))
set_fill(line, BORDER)

rows = [
    ("구성", "파일 하나, 임베디드", "PostgreSQL 확장, 압축/하이퍼테이블"),
    ("의존성", "libpq 전혀 링크 안 됨(ldd 확인)", "GW2_CrossPlatformCore의 libpq 래퍼 재사용"),
    ("검증 상태", "실저장까지 실측 검증 완료", "코드는 있으나 로컬 미설치로 미검증"),
]
ry = 4.03
for name, sqlite_v, ts_v in rows:
    simple_text(s, Inches(col_x[0]), Inches(ry), Inches(col_w[0]), Inches(0.34), name, 8, GRAY_TEXT)
    simple_text(s, Inches(col_x[1]), Inches(ry), Inches(col_w[1]), Inches(0.34), sqlite_v, 7.5, GREEN)
    simple_text(s, Inches(col_x[2]), Inches(ry), Inches(col_w[2]), Inches(0.34), ts_v, 7.5, GRAY_TEXT)
    ry += 0.37


# =========================================================
# Slide 5 — Highlight: ARIA→AES 마이그레이션
# =========================================================
s = add_slide()
page_header(s, "// Design Decision Highlight", "ARIA-CBC+HMAC → AES-GCM 마이그레이션", None, page=5)

simple_text(s, Inches(0.25), Inches(1.30), Inches(4.6), Inches(0.22), "// 마이그레이션 이유", 8, GRAY_MUTED, font=FONT_CODE)
reasons = [
    ("구조적 안전성", "CBC+HMAC 수동 조합은 순서를 잘못 짜면 패딩 오라클 재발 위험. GCM은 AEAD라 조합 실수 자체가 불가능."),
    ("단일 검증 경로", "구간마다 다른 암호는 검증 대상도 2배. AES-GCM 테스트 9개가 이제 두 구간 모두 커버."),
    ("무손실 전환", "IPayloadSealer 추상화 덕분에 세션/프레이밍 코드 변경 없이 주입 구현체만 교체."),
]
ry = Inches(1.55)
for title, desc in reasons:
    dot = rect(s, Inches(0.25), ry + Inches(0.06), Inches(0.08), Inches(0.08), MSO_SHAPE.OVAL)
    set_fill(dot, ACCENT)
    multi_run_text(s, Inches(0.42), ry, Inches(4.4), Inches(0.55),
                   [(title + " — ", DARK, True), (desc, GRAY_TEXT, False)], 8.3, line_spacing=1.2)
    ry += Inches(0.62)

callout = rect(s, Inches(0.25), Inches(3.55), Inches(4.6), Inches(1.35), MSO_SHAPE.ROUNDED_RECTANGLE)
callout.adjustments[0] = 0.05
set_border(callout, ACCENT, 0.75, fill=CALLOUT_BG)
multi_run_text(s, Inches(0.40), Inches(3.66), Inches(4.3), Inches(1.15),
               [("ARIA-256-CBC+HMAC 구현·검증 경험(Encrypt-then-MAC, 패딩 오라클 대응)은 코드·문서로 "
                 "보존 — ", ACCENT, False),
                ("Docs/ARIA_TO_AES_MIGRATION.md", ACCENT, True),
                ("에 정리.", ACCENT, False)], 9, line_spacing=1.3)

code_block(s, Inches(5.10), Inches(1.30), Inches(4.65), Inches(3.60), [
    [("// Collector/main.cpp — 수정 전/후", CODE_CMT)],
    [("", CODE_TEXT)],
    [("- AriaCipher::Key encKey = ...aria.key", CODE_RED)],
    [("- HmacUtil::Key macKey = ...hmac.key", CODE_RED)],
    [("+ AesGcmCipher::Key agentCollectorKey =", CODE_GREEN)],
    [("+     LoadKeyFromHexFile(", CODE_GREEN)],
    [("+         \"certs/agent_collector_aes.key\");", CODE_GREEN)],
    [("", CODE_TEXT)],
    [("  auto session = std::make_shared<ApmSession>(", CODE_TEXT)],
    [("      std::move(socket), sslContext,", CODE_TEXT)],
    [("      SessionMode::Server,", CODE_TEXT)],
    [("- std::make_unique<SecurePayload>(encKey, macKey));", CODE_RED)],
    [("+ std::make_unique<AesGcmPayload>(agentCollectorKey));", CODE_GREEN)],
    [("", CODE_TEXT)],
    [("// AriaCipher/HmacUtil/SecurePayload는 삭제하지", CODE_CMT)],
    [("// 않고 코드에 보존(참고용, 실행 경로 미사용)", CODE_CMT)],
])


# =========================================================
# Slide 6 — Highlight: Windows 크로스플랫폼 검증
# =========================================================
s = add_slide()
page_header(s, "// Cross-Platform Highlight", "Windows 크로스플랫폼 검증", "SAC(Smart App Control)가 막은 실행을 정적 링크로 우회", page=6)

simple_text(s, Inches(0.25), Inches(1.30), Inches(4.6), Inches(0.22), "// 이식하며 겪은 실제 이슈", 8, GRAY_MUTED, font=FONT_CODE)
win_issues = [
    ("ResourceCollector 이식", "Linux /proc 전용 → GetSystemTimes/GlobalMemoryStatusEx/GetIfTable2(WinAPI)로 신규 작성."),
    ("TCP_INFO 이식", "getsockopt(TCP_INFO) → WSAIoctl(SIO_TCP_INFO). API가 안 주는 필드(RTT 변동성 등)는 근사치 대신 0으로 정직하게 처리."),
    ("빌드 환경", "protobuf 버전 불일치(.pb.cc 재생성), MSVC 한글 소스 인코딩(/utf-8 옵션)."),
]
ry = Inches(1.55)
for title, desc in win_issues:
    dot = rect(s, Inches(0.25), ry + Inches(0.06), Inches(0.08), Inches(0.08), MSO_SHAPE.OVAL)
    set_fill(dot, ACCENT)
    multi_run_text(s, Inches(0.42), ry, Inches(4.4), Inches(0.62),
                   [(title + " — ", DARK, True), (desc, GRAY_TEXT, False)], 8.3, line_spacing=1.2)
    ry += Inches(0.68)

callout = rect(s, Inches(0.25), Inches(3.75), Inches(4.6), Inches(1.15), MSO_SHAPE.ROUNDED_RECTANGLE)
callout.adjustments[0] = 0.05
set_border(callout, RED, 0.75, fill=RED_BG)
multi_run_text(s, Inches(0.40), Inches(3.86), Inches(4.3), Inches(0.95),
               [("발견: ", RED, True),
                ("Windows 11 Smart App Control이 로컬에서 새로 빌드한(서명 안 된) vcpkg DLL의 "
                 "로딩 자체를 코드 무결성 정책으로 차단 — CodeIntegrity 이벤트 로그로 원인 확정.",
                 RED, False)], 8.5, line_spacing=1.25)

code_block(s, Inches(5.10), Inches(1.30), Inches(4.65), Inches(3.60), [
    [("// Before — 동적 링크: SAC가 DLL 로딩 차단", CODE_CMT)],
    [("cmake -B build -S . \\", CODE_TEXT)],
    [("    -DVCPKG_TARGET_TRIPLET=x64-windows", CODE_TEXT)],
    [("", CODE_TEXT)],
    [("// CodeIntegrity: Agent.exe가", CODE_RED)],
    [("// libprotobufd.dll 로딩 차단 - 응답 없음", CODE_RED)],
    [("", CODE_TEXT)],
    [("// After — 정적 링크: 외부 DLL 의존성 제거", CODE_CMT)],
    [("cmake -B build-static -S . \\", CODE_TEXT)],
    [("    -DVCPKG_TARGET_TRIPLET=x64-windows-static \\", CODE_TEXT)],
    [("    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded...", CODE_TEXT)],
    [("", CODE_TEXT)],
    [("// Collector.exe/Agent.exe 외부 DLL 0개", CODE_GREEN)],
    [("// → TLS+AES-GCM 통신, 실측 리소스 수집 정상", CODE_GREEN)],
    [("//   동작(6+ 사이클, 크래시 없음)", CODE_GREEN)],
])

# =========================================================
# Slide 7 — Bug #1
# =========================================================
s = add_slide()
page_header(s, "// Bug #1", "Razor HTML 인코딩 — JS 타임스탬프 파싱 실패", "대시보드 초기 렌더링에서 차트가 빈 그래프로 뜸", page=7)

bug_panel(s, Inches(0.25), Inches(1.18), Inches(4.65), Inches(3.55), "✕", "문제 — ISO 문자열의 '+'가 HTML 인코딩으로 깨짐", RED, RED_BG,
          [
              [("// 수정 전 - ISO 문자열을 그대로 JS에 삽입", CODE_CMT)],
              [("const initialPoints = [", CODE_TEXT)],
              [("  @foreach (var m in Model...)", CODE_TEXT)],
              [("  { t: \"", CODE_TEXT), ("@m.Ts...ToString(\"o\")", CODE_RED), ("\", ...},", CODE_TEXT)],
              [("];", CODE_TEXT)],
              [("", CODE_TEXT)],
              [("// Razor @ 표현식은 <script> 안이라도", CODE_CMT)],
              [("// 항상 HTML 인코딩됨: \"+09:00\" → \"&#x2B;09:00\"", CODE_CMT)],
              [("// → new Date(...) 파싱 실패", CODE_CMT)],
          ],
          note_runs=[("실제 렌더링 결과를 curl로 확인하는 과정에서 발견 — 설계 검토만으로는 못 잡는 유형.", GRAY_TEXT, False)])

bug_panel(s, Inches(5.10), Inches(1.18), Inches(4.65), Inches(3.55), "✓", "수정 — 유닉스 밀리초(숫자)로 전달", GREEN, GREEN_BG,
          [
              [("// 숫자는 인코딩할 특수문자가 없음", CODE_CMT)],
              [("const initialPoints = [", CODE_TEXT)],
              [("  @foreach (var m in Model...)", CODE_TEXT)],
              [("  { t: ", CODE_TEXT), ("@m.Ts.ToUnixTimeMilliseconds()", CODE_GREEN), (",", CODE_TEXT)],
              [("    cpu: @m.CpuUsagePercent...},", CODE_TEXT)],
              [("];", CODE_TEXT)],
              [("", CODE_TEXT)],
              [("// new Date(p.t)는 밀리초 숫자를 그대로 받음", CODE_CMT)],
              [("// → 이후 코드는 수정 불필요", CODE_CMT)],
          ],
          note_runs=[("이후 \"실제 실행/렌더링으로 검증\"을 모든 단계의 기본 절차로 삼는 계기가 됨.", GRAY_TEXT, False)])


# =========================================================
# Slide 8 — Bug #2 + 요약 표
# =========================================================
s = add_slide()
page_header(s, "// Bug #2", "TLS 핸드셰이크 모드 파라미터 누락", "Agent(client) 모드가 미정의 동작으로 실질 동작 안 함", page=8)

bug_panel(s, Inches(0.25), Inches(1.18), Inches(4.65), Inches(2.15), "✕", "문제 — mode 파라미터 없이 _mode 미정의", RED, RED_BG,
          [
              [("// 발견 당시 - 생성자에 mode 파라미터 없음", CODE_CMT)],
              [("ApmSession::ApmSession(socket, sslContext)", CODE_TEXT)],
              [("  : _sslStream(move(socket), sslContext)", CODE_TEXT)],
              [("  // ", CODE_CMT), ("_mode 미초기화 → 미정의 동작", CODE_RED)],
              [("{ }", CODE_TEXT)],
              [("", CODE_TEXT)],
              [("// handshakeType 계산만 하고 실제로는 안 씀", CODE_CMT)],
              [("async_handshake(", CODE_TEXT), ("stream_base::server", CODE_RED), (", ...);", CODE_TEXT)],
          ])

bug_panel(s, Inches(5.10), Inches(1.18), Inches(4.65), Inches(2.15), "✓", "수정 — mode를 받아 실제로 사용", GREEN, GREEN_BG,
          [
              [("ApmSession::ApmSession(socket, sslContext,", CODE_TEXT)],
              [("    SessionMode mode)", CODE_TEXT)],
              [("  : _sslStream(...), ", CODE_TEXT), ("_mode(mode)", CODE_GREEN)],
              [("{ }", CODE_TEXT)],
              [("", CODE_TEXT)],
              [("auto handshakeType = (_mode == Server)", CODE_TEXT)],
              [("    ? stream_base::server : stream_base::client;", CODE_TEXT)],
              [("async_handshake(", CODE_TEXT), ("handshakeType", CODE_GREEN), (", ...);", CODE_TEXT)],
          ],
          note_runs=[("※ 수정 시점(2026-07-14) 스냅샷 — 이후 IPayloadSealer 추상화로 생성자에 sealer 파라미터가 하나 더 붙음(현재 4개).", GRAY_BODY, False)])

# 요약 표 (HTML의 5건 전부 반영)
table_box = rect(s, Inches(0.25), Inches(3.42), Inches(9.5), Inches(1.83), MSO_SHAPE.ROUNDED_RECTANGLE)
table_box.adjustments[0] = 0.03
set_border(table_box, BORDER, 0.75, fill=CARD_BG)
simple_text(s, Inches(0.40), Inches(3.48), Inches(9.2), Inches(0.16), "// 그 외 적용 검증 중 발견한 버그", 7.3, GRAY_BODY, font=FONT_CODE)

cols = [0.40, 2.55, 6.80]
widths = [2.05, 4.15, 2.90]
hdrs = ["항목", "증상 → 조치", "발견 방법"]
for cx_, w_, h_ in zip(cols, widths, hdrs):
    simple_text(s, Inches(cx_), Inches(3.68), Inches(w_), Inches(0.16), h_, 6.8, GRAY_BODY, bold=True)
line = rect(s, Inches(0.40), Inches(3.86), Inches(9.20), Pt(0.5))
set_fill(line, BORDER)
rows7 = [
    ("ApmSession 0바이트", "빈 파일 그대로 컴파일 성공 → 내용 재작성", "nm 심볼 직접 확인"),
    ("Storage 계층 3종", "include 오타/빈 CMakeLists → 3건 직접 수정", "클린 빌드 재검증"),
    ("ComputeNetworkUsage", "redefinition 컴파일 에러 → 중복 삭제+ComputeCpuUsage 복원", "클린 빌드 재검증"),
    ("TCP_INFO 연동(6-5)", "링크/컴파일 에러 → 파일 4개 복원+타입명 오타 수정", "nm 심볼 검증"),
    ("ManifestEmbeddedFileProvider", "정적 리소스 404 → 패키지 참조 추가", "curl 실제 렌더링 확인"),
]
ry = 3.92
for a, b, c in rows7:
    simple_text(s, Inches(0.40), Inches(ry), Inches(2.05), Inches(0.23), a, 6.8, GRAY_TEXT)
    simple_text(s, Inches(2.55), Inches(ry), Inches(4.15), Inches(0.23), b, 6.8, GRAY_TEXT)
    simple_text(s, Inches(6.80), Inches(ry), Inches(2.90), Inches(0.23), c, 6.8, GRAY_TEXT)
    ry += 0.235


# =========================================================
# Slide 9 — 실측 지표
# =========================================================
s = add_slide()
page_header(s, "// Measured, Not Assumed", "실측 지표", '"가볍다"는 주장을 수치로 뒷받침 — strace/proc 실측과 테스트 결과.', page=9)

table_box = rect(s, Inches(0.25), Inches(1.25), Inches(6.0), Inches(3.95), MSO_SHAPE.ROUNDED_RECTANGLE)
table_box.adjustments[0] = 0.03
set_border(table_box, BORDER, 0.75, fill=WHITE)
simple_text(s, Inches(0.40), Inches(1.35), Inches(5.7), Inches(0.20), "// syscall 실측 (strace -c, 5분 정상 동작)", 7.5, GRAY_BODY, font=FONT_CODE)

hdr_y = 1.62
for cx_, w_, t_ in [(0.40, 2.6, "항목"), (3.10, 1.3, "횟수"), (4.50, 1.55, "비고")]:
    simple_text(s, Inches(cx_), Inches(hdr_y), Inches(w_), Inches(0.20), t_, 7.5, GRAY_BODY, bold=True)
line = rect(s, Inches(0.40), Inches(1.84), Inches(5.70), Pt(0.5))
set_fill(line, BORDER)

metrics_rows = [
    ("epoll_wait", "155", "스핀 없이 블로킹", False),
    ("read", "460", "TLS 수신+/proc 읽기", False),
    ("openat / close", "311 / 309", "매 사이클 재오픈(32%)", False),
    ("sendto / write", "76 / 77", "TLS 전송", False),
    ("getsockopt", "75", "TCP_INFO 조회", False),
    ("statfs", "74", "디스크 사용량 조회(statvfs)", False),
    ("전체 syscall", "1,921회", "19.4ms, 대기가 압도적", True),
    ("RSS", "13.8MB 고정", "누수 없음(20샘플)", True),
    ("CPU 평균", "0.017%", "유휴 대기가 거의 전부", True),
]
ry = 1.90
for name, val, note, strong in metrics_rows:
    color = GREEN if strong else GRAY_TEXT
    simple_text(s, Inches(0.40), Inches(ry), Inches(2.6), Inches(0.26), name, 8, GRAY_TEXT, bold=strong)
    simple_text(s, Inches(3.10), Inches(ry), Inches(1.3), Inches(0.26), val, 8, color, bold=strong, font=FONT_CODE)
    simple_text(s, Inches(4.50), Inches(ry), Inches(1.55), Inches(0.26), note, 7.2, GRAY_BODY)
    ry += 0.335

right = rect(s, Inches(6.45), Inches(1.25), Inches(3.30), Inches(3.95), MSO_SHAPE.ROUNDED_RECTANGLE)
right.adjustments[0] = 0.03
set_border(right, ACCENT, 0.75, fill=CALLOUT_BG)
simple_text(s, Inches(6.65), Inches(1.42), Inches(2.9), Inches(0.24), "// 테스트", 8, ACCENT, font=FONT_CODE)
simple_text(s, Inches(6.65), Inches(1.66), Inches(2.9), Inches(0.60), "17 / 17 통과", 22, ACCENT, bold=True)
multi_run_text(s, Inches(6.65), Inches(2.30), Inches(2.9), Inches(2.6),
               [("C++ GoogleTest 9개(AesGcmCipher/Payload) + ", GRAY_TEXT, False),
                (".NET xUnit 8개(ReadExactAsync/DecryptAndParse).\n\n", GRAY_TEXT, False),
                ("Agent↔Collector도 AES-GCM으로 통일되면서, 이 9개가 두 구간 암호화 경로 모두를 "
                 "커버 — 이전엔 AES-GCM만 테스트 있고 ARIA는 없는 비대칭이 있었음.", GRAY_TEXT, False)],
               8.3, line_spacing=1.3)


# =========================================================
# Slide 10 — 대시보드 스크린샷 (플레이스홀더, 다크)
# =========================================================
s = add_slide()
bg = s.background
bg.fill.solid()
bg.fill.fore_color.rgb = RGBColor(0x0D, 0x11, 0x17)
left_bar(s, 0.07)
simple_text(s, Inches(0.25), Inches(0.28), Inches(9.4), Inches(0.30), "APM_Console 대시보드 실행 화면", 14, RGBColor(0xE6, 0xED, 0xF3), bold=True)

placeholder = rect(s, Inches(0.60), Inches(0.75), Inches(8.80), Inches(4.10), MSO_SHAPE.ROUNDED_RECTANGLE)
placeholder.adjustments[0] = 0.02
placeholder.fill.solid()
placeholder.fill.fore_color.rgb = RGBColor(0x16, 0x1B, 0x22)
placeholder.line.color.rgb = RGBColor(0x30, 0x36, 0x3D)
placeholder.line.width = Pt(1.0)

tf = placeholder.text_frame
tf.word_wrap = True
tf.vertical_anchor = MSO_ANCHOR.MIDDLE
p = tf.paragraphs[0]
p.alignment = PP_ALIGN.CENTER
r = p.add_run()
set_run(r, "[ /apm/dashboard 스크린샷 삽입 위치 ]", 16, RGBColor(0x8B, 0x94, 0x9E), bold=True)
p2 = tf.add_paragraph()
p2.alignment = PP_ALIGN.CENTER
r2 = p2.add_run()
set_run(r2, "Chart.js 그래프 5개 + SignalR 실시간 표(11컬럼) — Collector/Agent/APM_Console 실행 후 캡처", 10, RGBColor(0x6E, 0x76, 0x81))

simple_text(s, Inches(8.70), Inches(5.35), Inches(1.10), Inches(0.20), "10 / 12", 7.5, RGBColor(0x6E, 0x76, 0x81), align=PP_ALIGN.RIGHT)


# =========================================================
# Slide 11 — Tech Stack
# =========================================================
s = add_slide()
page_header(s, "// Tech Stack", "기술 스택", "프로젝트에서 실제 사용하고 검증한 기술.", page=11)

groups = [
    ("네트워크 코어 (C++)", ACCENT, ["C++20, Standalone Asio", "OS별 비동기 I/O 추상화(IOCP/epoll)", "CMake, GoogleTest, vcpkg",
                                     "Protobuf 직렬화+리플렉션", "메시지 프레이밍(PacketHeader)", "Windows API(GetSystemTimes/GetIfTable2/SIO_TCP_INFO)"]),
    ("보안 설계", RED, ["TLS(asio::ssl, OpenSSL)", "AES-256-GCM(AEAD, 현재 사용)", "ARIA-256-CBC+HMAC(구현 경험)",
                       "패딩 오라클 이해/대응", "최소 권한(setuid/setgid)"]),
    ("데이터 / 저장", GREEN, ["SQLite3", "PostgreSQL/TimescaleDB(선택형)", "IMetricStore 추상화", "컴파일 타임 백엔드 선택"]),
    ("웹 대시보드 (.NET)", AMBER, [".NET 8, ASP.NET Core MVC", "Entity Framework Core", "SignalR 실시간 푸시",
                                  "AssemblyLoadContext 플러그인", "xUnit, Chart.js"]),
]
cx = Inches(0.25)
cw = Inches(2.30)
for title, color, items in groups:
    card = rect(s, cx, Inches(1.18), cw, Inches(2.85))
    set_border(card, BORDER, 0.75, fill=WHITE)
    head = rect(s, cx, Inches(1.18), cw, Inches(0.35))
    set_fill(head, color)
    simple_text(s, cx + Inches(0.10), Inches(1.18), cw - Inches(0.2), Inches(0.35), title, 9, WHITE, bold=True, valign=MSO_ANCHOR.MIDDLE)
    iy = 1.60
    for item in items:
        simple_text(s, cx + Inches(0.12), Inches(iy), cw - Inches(0.24), Inches(0.34), f"·  {item}", 7.7, GRAY_TEXT)
        iy += 0.375
    cx += cw + Inches(0.15)

callout = rect(s, Inches(0.25), Inches(4.28), Inches(9.5), Inches(0.78), MSO_SHAPE.ROUNDED_RECTANGLE)
callout.adjustments[0] = 0.06
set_border(callout, ACCENT, 0.75, fill=CALLOUT_BG)
multi_run_text(s, Inches(0.40), Inches(4.34), Inches(9.2), Inches(0.26),
               [("IPayloadSealer", ACCENT, True), (" → ARIA/AES 두 구현체를 세션 코드 변경 없이 교체 가능한 구조", ACCENT, False)],
               10)
multi_run_text(s, Inches(0.40), Inches(4.60), Inches(9.2), Inches(0.22),
               [("실제 마이그레이션이 main.cpp 몇 줄로 끝난 것 자체가 이 추상화의 값을 증명.", ACCENT, False)], 8.5)


# =========================================================
# Slide 12 — Closing / Contact
# =========================================================
s = add_slide()
left_bar(s, 0.18)
simple_text(s, Inches(0.35), Inches(0.60), Inches(5.5), Inches(0.75), "감사합니다", 34, DARK, bold=True)
simple_text(s, Inches(0.35), Inches(1.36), Inches(5.5), Inches(0.30), "APM/옵저버빌리티 에이전트 개발자  김서현", 13, GRAY_TEXT)

divider = rect(s, Inches(0.35), Inches(1.76), Inches(5.5), Pt(0.75))
set_fill(divider, BORDER)

box = rect(s, Inches(0.35), Inches(1.90), Inches(5.5), Inches(0.70), MSO_SHAPE.ROUNDED_RECTANGLE)
box.adjustments[0] = 0.08
set_border(box, ACCENT, 0.75, fill=CALLOUT_BG)
multi_run_text(s, Inches(0.50), Inches(1.96), Inches(5.2), Inches(0.26),
               [("C++20 · Standalone Asio · TLS/AES-256-GCM", ACCENT, False)], 10)
multi_run_text(s, Inches(0.50), Inches(2.22), Inches(5.2), Inches(0.22),
               [(".NET 8 · SignalR · 도메인별 RCL 플러그인", ACCENT, False)], 8.5)

simple_text(s, Inches(0.35), Inches(2.78), Inches(5.5), Inches(0.30), "shkim4548@gmail.com", 13, ACCENT)
simple_text(s, Inches(0.35), Inches(3.08), Inches(5.5), Inches(0.26),
            "소스 코드 열람이 필요하신 경우 이메일로 요청해 주시면 공유해 드립니다.", 8.5, GRAY_BODY)

stat(s, Inches(0.35), Inches(3.50), "5", "수집 지표")
stat(s, Inches(1.73), Inches(3.50), "17", "테스트 통과")
stat(s, Inches(3.11), Inches(3.50), "9", "설계 결정")
stat(s, Inches(4.49), Inches(3.50), "7", "버그 발견/수정")

right = rect(s, Inches(6.10), Inches(0.28), Inches(3.60), Inches(5.05), MSO_SHAPE.ROUNDED_RECTANGLE)
right.adjustments[0] = 0.02
set_border(right, BORDER2, 0.75, fill=CARD_BG)
simple_text(s, Inches(6.30), Inches(2.50), Inches(3.20), Inches(0.5), "[ 스크린샷/로고 자리 ]", 12, GRAY_MUTED,
            align=PP_ALIGN.CENTER, valign=MSO_ANCHOR.MIDDLE)

simple_text(s, Inches(8.70), Inches(5.35), Inches(1.10), Inches(0.20), "12 / 12", 7.5, GRAY_MUTED, align=PP_ALIGN.RIGHT)


import os
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "김서현_APM_포트폴리오_v1.pptx")
prs.save(out_path)
print("saved:", out_path)
print("slides:", len(prs.slides._sldIdLst))
