// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #354 D2 verify set: PDFs whose one visual page is a receipt, a floor plan, a
// chart... (20 kinds, 6 documents each), drawn with CoreGraphics / CoreText
// among pages of prose. Names are neutral ("doc_v###.pdf"); the label of each
// goes to labels.tsv beside them, so no query can match a file name.
//
//   swift make_doc_pages.swift <out dir> <photos dir (JPEGs)>
// Then: ai-bench --documents --doc-index 2 --top 10 --query "a receipt" ... and
// score_doc_pages.py <ai-bench output> <labels.tsv> (the 20 queries are its table).
import AppKit
import CoreText
import Foundation
import ImageIO

let out = URL(fileURLWithPath: CommandLine.arguments[1])
let photoDir = URL(fileURLWithPath: CommandLine.arguments[2])
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

struct Rng: RandomNumberGenerator {
  var s: UInt64
  mutating func next() -> UInt64 { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s }
}
var rng = Rng(s: 0x5EED_D2D2)
func r(_ a: Double, _ b: Double) -> Double { Double.random(in: a...b, using: &rng) }
func ri(_ a: Int, _ b: Int) -> Int { Int.random(in: a...b, using: &rng) }
func pick<T>(_ xs: [T]) -> T { xs.randomElement(using: &rng)! }

let words = ["lease", "tenant", "invoice", "payment", "quarterly", "summary", "report", "contract", "schedule",
             "delivery", "account", "balance", "insurance", "policy", "renewal", "meeting", "minutes", "budget",
             "project", "review", "garden", "kitchen", "holiday", "travel", "receipt", "warranty", "manual",
             "engine", "battery", "service", "customer", "supplier", "order", "shipment", "deadline", "approval"]
func sentence(_ n: Int) -> String {
  let s = (0..<n).map { _ in pick(words) }.joined(separator: " ")
  return s.prefix(1).uppercased() + s.dropFirst() + "."
}
func paragraph() -> String { (0..<ri(3, 6)).map { _ in sentence(ri(8, 16)) }.joined(separator: " ") }

func color(_ r: Double, _ g: Double, _ b: Double, _ a: Double = 1) -> CGColor { CGColor(red: r, green: g, blue: b, alpha: a) }
let palette: [CGColor] = [color(0.20, 0.45, 0.80), color(0.90, 0.45, 0.15), color(0.25, 0.65, 0.35),
                          color(0.80, 0.25, 0.30), color(0.55, 0.35, 0.70), color(0.95, 0.75, 0.20),
                          color(0.30, 0.70, 0.75)]

func text(_ ctx: CGContext, _ s: String, _ x: Double, _ y: Double, size: Double = 11, font: String = "Helvetica",
          color c: CGColor = color(0, 0, 0)) {
  let f = CTFontCreateWithName(font as CFString, size, nil)
  let attr = NSAttributedString(string: s, attributes: [.font: f, .foregroundColor: c])
  let line = CTLineCreateWithAttributedString(attr)
  ctx.textPosition = CGPoint(x: x, y: y)
  CTLineDraw(line, ctx)
}
func prose(_ ctx: CGContext, _ box: CGRect) {
  let font = CTFontCreateWithName("Helvetica" as CFString, 11, nil)
  let t = (0..<4).map { _ in paragraph() }.joined(separator: "\n\n")
  let attr = NSAttributedString(string: t, attributes: [.font: font])
  let setter = CTFramesetterCreateWithAttributedString(attr)
  let frame = CTFramesetterCreateFrame(setter, CFRange(location: 0, length: 0),
                                       CGPath(rect: box.insetBy(dx: 72, dy: 72), transform: nil), nil)
  CTFrameDraw(frame, ctx)
}
func fill(_ ctx: CGContext, _ rect: CGRect, _ c: CGColor) { ctx.setFillColor(c); ctx.fill(rect) }
func stroke(_ ctx: CGContext, _ rect: CGRect, _ c: CGColor = color(0, 0, 0), _ w: Double = 1) {
  ctx.setStrokeColor(c); ctx.setLineWidth(w); ctx.stroke(rect)
}
func line(_ ctx: CGContext, _ pts: [(Double, Double)], _ c: CGColor = color(0, 0, 0), _ w: Double = 1) {
  ctx.setStrokeColor(c); ctx.setLineWidth(w); ctx.beginPath()
  ctx.move(to: CGPoint(x: pts[0].0, y: pts[0].1))
  for p in pts.dropFirst() { ctx.addLine(to: CGPoint(x: p.0, y: p.1)) }
  ctx.strokePath()
}

typealias Draw = (CGContext, CGRect) -> Void

// ---- the 20 kinds ------------------------------------------------------------------------

let receipt: Draw = { ctx, b in
  let w = r(220, 280), h = r(560, 700), x = (b.width - w) / 2, y = (b.height - h) / 2
  fill(ctx, CGRect(x: x, y: y, width: w, height: h), color(0.98, 0.98, 0.96))
  stroke(ctx, CGRect(x: x, y: y, width: w, height: h), color(0.8, 0.8, 0.8))
  var cy = y + h - 40
  text(ctx, pick(["CORNER MARKET", "CAFE LUMEN", "HARDWARE PLUS", "FRESH GROCER", "CITY PHARMACY"]), x + 30, cy, size: 16, font: "Courier-Bold")
  cy -= 18; text(ctx, "\(ri(10, 99)) High Street  Tel \(ri(1000, 9999)) \(ri(100000, 999999))", x + 20, cy, size: 8, font: "Courier")
  cy -= 14; text(ctx, "\(ri(1, 28))/\(ri(1, 12))/2026  \(ri(8, 20)):\(ri(10, 59))", x + 20, cy, size: 9, font: "Courier")
  cy -= 10; line(ctx, [(x + 10, cy), (x + w - 10, cy)], color(0.3, 0.3, 0.3), 0.5)
  var total = 0.0
  for _ in 0..<ri(8, 18) {
    cy -= 16
    let p = r(0.5, 30); total += p
    text(ctx, pick(["MILK 1L", "BREAD", "COFFEE", "APPLES", "BATTERIES", "SOAP", "PASTA", "EGGS x12", "TEA",
                    "TAPE", "SCREWS", "BANANAS", "CHEESE", "WATER", "RICE"]), x + 16, cy, size: 10, font: "Courier")
    text(ctx, String(format: "%.2f", p), x + w - 60, cy, size: 10, font: "Courier")
  }
  cy -= 12; line(ctx, [(x + 10, cy), (x + w - 10, cy)], color(0.3, 0.3, 0.3), 0.5)
  cy -= 18; text(ctx, "TOTAL", x + 16, cy, size: 12, font: "Courier-Bold")
  text(ctx, String(format: "%.2f", total), x + w - 70, cy, size: 12, font: "Courier-Bold")
  cy -= 16; text(ctx, "CARD  **** \(ri(1000, 9999))", x + 16, cy, size: 9, font: "Courier")
  cy -= 24; text(ctx, "THANK YOU FOR SHOPPING", x + 24, cy, size: 9, font: "Courier")
}

let floorPlan: Draw = { ctx, b in
  let x0 = 70.0, y0 = r(150, 220), w = b.width - 140, h = r(380, 480)
  ctx.setStrokeColor(color(0, 0, 0)); ctx.setLineWidth(7)
  ctx.stroke(CGRect(x: x0, y: y0, width: w, height: h))
  ctx.setLineWidth(4)
  let vx = x0 + w * r(0.35, 0.6), hy = y0 + h * r(0.4, 0.6)
  line(ctx, [(vx, y0), (vx, y0 + h)], color(0, 0, 0), 4)
  line(ctx, [(x0, hy), (vx, hy)], color(0, 0, 0), 4)
  let hx = vx + (x0 + w - vx) * r(0.4, 0.6)
  line(ctx, [(hx, hy), (hx, y0 + h)], color(0, 0, 0), 4)
  line(ctx, [(vx, hy), (x0 + w, hy)], color(0, 0, 0), 4)
  // Doors: a gap and a quarter arc.
  for (dx, dy) in [(vx, hy - 40), (x0 + 60, hy), (hx + 30, hy), (vx, hy + 50)] {
    fill(ctx, CGRect(x: dx - 3, y: dy - 3, width: 34, height: 6), color(1, 1, 1))
    ctx.setLineWidth(1); ctx.beginPath()
    ctx.addArc(center: CGPoint(x: dx, y: dy), radius: 30, startAngle: 0, endAngle: .pi / 2, clockwise: false)
    ctx.strokePath()
  }
  let rooms = ["KITCHEN", "LIVING ROOM", "BEDROOM", "BATH", "HALL", "DINING", "STUDY"].shuffled(using: &rng)
  text(ctx, rooms[0], x0 + 30, y0 + 40, size: 10)
  text(ctx, rooms[1], x0 + 30, hy + 40, size: 10)
  text(ctx, rooms[2], vx + 20, y0 + 40, size: 10)
  text(ctx, rooms[3], vx + 20, hy + 40, size: 10)
  text(ctx, rooms[4], hx + 20, hy + 40, size: 10)
  // Dimension line.
  line(ctx, [(x0, y0 - 30), (x0 + w, y0 - 30)], color(0.3, 0.3, 0.3), 0.6)
  text(ctx, String(format: "%.1f m", r(9, 15)), x0 + w / 2 - 15, y0 - 45, size: 9)
  text(ctx, "Ground floor plan  1:100", x0, y0 + h + 30, size: 12, font: "Helvetica-Bold")
}

func axes(_ ctx: CGContext, _ x0: Double, _ y0: Double, _ w: Double, _ h: Double, grid: Bool) {
  if grid { for i in 1...5 { line(ctx, [(x0, y0 + h * Double(i) / 5), (x0 + w, y0 + h * Double(i) / 5)], color(0.85, 0.85, 0.85), 0.5) } }
  line(ctx, [(x0, y0 + h), (x0, y0), (x0 + w, y0)], color(0.2, 0.2, 0.2), 1.2)
  for i in 0...5 { text(ctx, "\(i * ri(10, 50))", x0 - 28, y0 + h * Double(i) / 5 - 3, size: 8) }
}

let barChart: Draw = { ctx, b in
  let x0 = 100.0, y0 = 200.0, w = b.width - 170, h = 380.0
  text(ctx, pick(["Sales by region", "Quarterly revenue", "Visitors per month", "Survey results"]), x0, y0 + h + 40, size: 16, font: "Helvetica-Bold")
  axes(ctx, x0, y0, w, h, grid: true)
  let n = ri(4, 9), bw = w / Double(n) * 0.6, c = pick(palette), multi = ri(0, 1) == 1
  for i in 0..<n {
    let bh = r(0.15, 0.95) * h, bx = x0 + w / Double(n) * (Double(i) + 0.2)
    fill(ctx, CGRect(x: bx, y: y0, width: bw, height: bh), multi ? palette[i % palette.count] : c)
    text(ctx, pick(["Q1", "Q2", "Q3", "Q4", "North", "South", "East", "West", "Jan", "Feb", "Mar"]), bx, y0 - 16, size: 8)
  }
}

let pieChart: Draw = { ctx, b in
  let c = CGPoint(x: b.width / 2 - 60, y: b.height / 2), rad = r(140, 190)
  text(ctx, pick(["Market share", "Budget breakdown", "Time spent", "Energy sources"]), 72, b.height - 110, size: 16, font: "Helvetica-Bold")
  let n = ri(3, 7); var parts = (0..<n).map { _ in r(1, 5) }; let sum = parts.reduce(0, +); parts = parts.map { $0 / sum }
  var a = Double.pi / 2
  for (i, p) in parts.enumerated() {
    ctx.setFillColor(palette[i % palette.count]); ctx.beginPath(); ctx.move(to: c)
    ctx.addArc(center: c, radius: rad, startAngle: a, endAngle: a - p * 2 * .pi, clockwise: true)
    ctx.closePath(); ctx.fillPath(); a -= p * 2 * .pi
    fill(ctx, CGRect(x: c.x + rad + 40, y: c.y + 60 - Double(i) * 22, width: 12, height: 12), palette[i % palette.count])
    text(ctx, "\(pick(["Solar", "Wind", "Rent", "Food", "Travel", "Other", "Coal", "Work", "Sleep"])) \(Int(p * 100))%", c.x + rad + 58, c.y + 61 - Double(i) * 22, size: 9)
  }
}

let lineGraph: Draw = { ctx, b in
  let x0 = 100.0, y0 = 200.0, w = b.width - 170, h = 380.0
  text(ctx, pick(["Temperature over time", "Stock price", "Website traffic", "Growth since 2019"]), x0, y0 + h + 40, size: 16, font: "Helvetica-Bold")
  axes(ctx, x0, y0, w, h, grid: true)
  for s in 0..<ri(1, 3) {
    var v = r(0.2, 0.7); var pts: [(Double, Double)] = []
    let n = ri(10, 24)
    for i in 0..<n { v = min(0.95, max(0.05, v + r(-0.12, 0.12))); pts.append((x0 + w * Double(i) / Double(n - 1), y0 + h * v)) }
    line(ctx, pts, palette[s], 2.2)
    for p in pts { ctx.setFillColor(palette[s]); ctx.fillEllipse(in: CGRect(x: p.0 - 3, y: p.1 - 3, width: 6, height: 6)) }
  }
}

let slide: Draw = { ctx, b in
  let c = pick(palette)
  fill(ctx, CGRect(x: 0, y: b.height - 110, width: b.width, height: 110), c)
  text(ctx, pick(["Our strategy for 2027", "Product roadmap", "Welcome to the team", "Results and next steps", "Agenda"]), 50, b.height - 72, size: 34, font: "Helvetica-Bold", color: color(1, 1, 1))
  for i in 0..<ri(3, 5) {
    ctx.setFillColor(c); ctx.fillEllipse(in: CGRect(x: 60, y: b.height - 175 - Double(i) * 52, width: 12, height: 12))
    text(ctx, pick(["Grow in new markets", "Ship the mobile app", "Hire ten engineers", "Cut costs by 15%", "Customer first", "Launch in spring"]), 90, b.height - 180 - Double(i) * 52, size: 22)
  }
  text(ctx, "\(ri(2, 30))", b.width - 50, 24, size: 12, color: color(0.5, 0.5, 0.5))
}

let sheetMusic: Draw = { ctx, b in
  text(ctx, pick(["Sonatina", "Minuet in G", "Etude No. \(ri(1, 12))", "Waltz"]), b.width / 2 - 60, b.height - 80, size: 20, font: "Times-Bold")
  for s in 0..<ri(7, 9) {
    let top = b.height - 140 - Double(s) * 78
    for l in 0..<5 { line(ctx, [(60, top - Double(l) * 7), (b.width - 60, top - Double(l) * 7)], color(0, 0, 0), 0.7) }
    text(ctx, "\u{1D11E}", 62, top - 34, size: 42, font: "Apple Symbols")
    var x = 110.0
    while x < b.width - 80 {
      let ny = top - 28 + Double(ri(-4, 6)) * 3.5
      ctx.setFillColor(color(0, 0, 0)); ctx.fillEllipse(in: CGRect(x: x, y: ny, width: 8, height: 6))
      line(ctx, [(x + 7.5, ny + 3), (x + 7.5, ny + 24)], color(0, 0, 0), 1)
      x += r(18, 30)
      if ri(0, 5) == 0 { line(ctx, [(x - 4, top), (x - 4, top - 28)], color(0, 0, 0), 1) }
    }
  }
}

let calendar: Draw = { ctx, b in
  let month = pick(["January", "March", "May", "July", "September", "November"])
  text(ctx, "\(month) 2027", 72, b.height - 110, size: 30, font: "Helvetica-Bold")
  let x0 = 60.0, y0 = 140.0, cw = (b.width - 120) / 7, ch = 95.0
  for (i, d) in ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"].enumerated() { text(ctx, d, x0 + cw * Double(i) + 6, y0 + ch * 5 + 10, size: 10, font: "Helvetica-Bold") }
  var day = 1 - ri(0, 6)
  for row in 0..<5 { for col in 0..<7 {
    let rect = CGRect(x: x0 + cw * Double(col), y: y0 + ch * Double(4 - row), width: cw, height: ch)
    if col >= 5 { fill(ctx, rect, color(0.94, 0.94, 0.97)) }
    stroke(ctx, rect, color(0.6, 0.6, 0.6), 0.7)
    if day >= 1 && day <= 31 { text(ctx, "\(day)", rect.minX + 5, rect.maxY - 15, size: 11) }
    if ri(0, 9) == 0 && day >= 1 { fill(ctx, CGRect(x: rect.minX + 4, y: rect.minY + 30, width: cw - 8, height: 14), pick(palette)) }
    day += 1
  } }
}

let crossword: Draw = { ctx, b in
  let n = 15, s = 26.0, x0 = (b.width - s * 15) / 2, y0 = b.height - 90 - s * 15
  var black = Set<Int>()
  for _ in 0..<ri(30, 40) { let i = ri(0, n * n - 1); black.insert(i); black.insert(n * n - 1 - i) }
  var num = 1
  for row in 0..<n { for col in 0..<n {
    let rect = CGRect(x: x0 + s * Double(col), y: y0 + s * Double(n - 1 - row), width: s, height: s)
    if black.contains(row * n + col) { fill(ctx, rect, color(0, 0, 0)) } else {
      stroke(ctx, rect, color(0, 0, 0), 0.8)
      if (row == 0 || col == 0 || black.contains(row * n + col - 1)) && ri(0, 2) == 0 { text(ctx, "\(num)", rect.minX + 2, rect.maxY - 8, size: 6); num += 1 }
    }
  } }
  text(ctx, "ACROSS", 60, y0 - 30, size: 10, font: "Helvetica-Bold")
  for i in 0..<5 { text(ctx, "\(i * 3 + 1). \(pick(words)) of the \(pick(words)) (\(ri(3, 9)))", 60, y0 - 46 - Double(i) * 13, size: 8) }
  text(ctx, "DOWN", 320, y0 - 30, size: 10, font: "Helvetica-Bold")
  for i in 0..<5 { text(ctx, "\(i * 2 + 2). \(pick(words)) \(pick(words)) (\(ri(3, 9)))", 320, y0 - 46 - Double(i) * 13, size: 8) }
}

let chessboard: Draw = { ctx, b in
  let s = r(52, 62), x0 = (b.width - s * 8) / 2, y0 = (b.height - s * 8) / 2
  let dark = pick([color(0.46, 0.59, 0.34), color(0.71, 0.53, 0.39), color(0.45, 0.45, 0.55)])
  for row in 0..<8 { for col in 0..<8 {
    fill(ctx, CGRect(x: x0 + s * Double(col), y: y0 + s * Double(row), width: s, height: s), (row + col) % 2 == 0 ? dark : color(0.93, 0.93, 0.82))
  } }
  let pieces = ["\u{2654}", "\u{2655}", "\u{2656}", "\u{2657}", "\u{2658}", "\u{2659}", "\u{265A}", "\u{265B}", "\u{265C}", "\u{265D}", "\u{265E}", "\u{265F}"]
  for _ in 0..<ri(10, 22) {
    text(ctx, pick(pieces), x0 + s * Double(ri(0, 7)) + s * 0.12, y0 + s * Double(ri(0, 7)) + s * 0.18, size: s * 0.75, font: "Apple Symbols")
  }
  for i in 0..<8 { text(ctx, String(UnicodeScalar(97 + i)!), x0 + s * Double(i) + s / 2 - 3, y0 - 16, size: 10) }
  text(ctx, "White to move. Mate in \(ri(2, 4)).", x0, y0 - 44, size: 12)
}

func box(_ ctx: CGContext, _ rect: CGRect, _ label: String, diamond: Bool = false, round: Bool = false) {
  ctx.setStrokeColor(color(0.1, 0.1, 0.1)); ctx.setLineWidth(1.5)
  ctx.setFillColor(diamond ? color(1, 0.95, 0.8) : color(0.85, 0.92, 1))
  let p = CGMutablePath()
  if diamond {
    p.move(to: CGPoint(x: rect.midX, y: rect.maxY)); p.addLine(to: CGPoint(x: rect.maxX, y: rect.midY))
    p.addLine(to: CGPoint(x: rect.midX, y: rect.minY)); p.addLine(to: CGPoint(x: rect.minX, y: rect.midY)); p.closeSubpath()
  } else { p.addRoundedRect(in: rect, cornerWidth: round ? 20 : 4, cornerHeight: round ? 20 : 4) }
  ctx.addPath(p); ctx.drawPath(using: .fillStroke)
  text(ctx, label, rect.midX - Double(label.count) * 3, rect.midY - 4, size: 10)
}
func arrow(_ ctx: CGContext, _ a: (Double, Double), _ b2: (Double, Double)) {
  line(ctx, [a, b2], color(0.1, 0.1, 0.1), 1.5)
  let ang = atan2(b2.1 - a.1, b2.0 - a.0)
  ctx.setFillColor(color(0.1, 0.1, 0.1)); ctx.beginPath(); ctx.move(to: CGPoint(x: b2.0, y: b2.1))
  ctx.addLine(to: CGPoint(x: b2.0 - 10 * cos(ang - 0.4), y: b2.1 - 10 * sin(ang - 0.4)))
  ctx.addLine(to: CGPoint(x: b2.0 - 10 * cos(ang + 0.4), y: b2.1 - 10 * sin(ang + 0.4))); ctx.closePath(); ctx.fillPath()
}

let flowchart: Draw = { ctx, b in
  let cx = b.width / 2, bw = 150.0, bh = 50.0
  var y = b.height - 120
  box(ctx, CGRect(x: cx - bw / 2, y: y, width: bw, height: bh), "Start", round: true)
  let steps = ri(3, 4)
  for i in 0..<steps {
    arrow(ctx, (cx, y), (cx, y - 40)); y -= 40 + bh + 20
    let isQ = i % 2 == 1
    box(ctx, CGRect(x: cx - bw / 2, y: y, width: bw, height: bh + (isQ ? 20 : 0)), isQ ? "Approved?" : pick(["Submit form", "Check stock", "Send invoice", "Review"]), diamond: isQ)
    if isQ {
      arrow(ctx, (cx + bw / 2, y + 35), (cx + bw / 2 + 80, y + 35))
      box(ctx, CGRect(x: cx + bw / 2 + 80, y: y + 10, width: 110, height: bh), "Reject")
      text(ctx, "No", cx + bw / 2 + 20, y + 42, size: 9); text(ctx, "Yes", cx + 6, y - 14, size: 9)
    }
  }
  arrow(ctx, (cx, y), (cx, y - 40)); y -= 40 + bh
  box(ctx, CGRect(x: cx - bw / 2, y: y, width: bw, height: bh), "End", round: true)
}

let map: Draw = { ctx, b in
  fill(ctx, b, color(0.95, 0.93, 0.87))
  for _ in 0..<ri(3, 5) {
    ctx.setFillColor(color(0.75, 0.87, 0.65)); ctx.fillEllipse(in: CGRect(x: r(0, b.width - 150), y: r(0, b.height - 150), width: r(60, 180), height: r(60, 160)))
  }
  var pts: [(Double, Double)] = []; var x = -20.0, y = r(100, b.height - 100)
  while x < b.width + 20 { pts.append((x, y)); x += r(30, 60); y += r(-40, 40) }
  line(ctx, pts, color(0.55, 0.75, 0.95), r(14, 24))
  for _ in 0..<ri(10, 16) {
    let horiz = ri(0, 1) == 0, p = r(30, horiz ? b.height - 30 : b.width - 30), wd = Double(pick([3, 3, 6]))
    line(ctx, horiz ? [(0, p), (b.width, p + r(-60, 60))] : [(p, 0), (p + r(-60, 60), b.height)], color(1, 1, 1), wd + 2)
    line(ctx, horiz ? [(0, p), (b.width, p + r(-60, 60))] : [(p, 0), (p + r(-60, 60), b.height)], wd > 4 ? color(0.98, 0.8, 0.4) : color(0.8, 0.8, 0.8), wd)
  }
  for _ in 0..<8 { text(ctx, pick(["High St", "Park Rd", "Mill Lane", "River Walk", "Station Rd", "Church St", "Kings Way"]), r(30, b.width - 100), r(30, b.height - 30), size: 8, color: color(0.3, 0.3, 0.3)) }
  ctx.setFillColor(color(0.85, 0.2, 0.2)); ctx.fillEllipse(in: CGRect(x: b.width / 2, y: b.height / 2, width: 14, height: 14))
  text(ctx, "N \u{2191}", b.width - 50, b.height - 40, size: 14, font: "Helvetica-Bold")
}

let qrCode: Draw = { ctx, b in
  let n = 29, s = r(8, 11), x0 = (b.width - s * Double(n)) / 2, y0 = (b.height - s * Double(n)) / 2 + 40
  func finder(_ cx: Int, _ cy: Int) {
    fill(ctx, CGRect(x: x0 + s * Double(cx), y: y0 + s * Double(cy), width: s * 7, height: s * 7), color(0, 0, 0))
    fill(ctx, CGRect(x: x0 + s * Double(cx + 1), y: y0 + s * Double(cy + 1), width: s * 5, height: s * 5), color(1, 1, 1))
    fill(ctx, CGRect(x: x0 + s * Double(cx + 2), y: y0 + s * Double(cy + 2), width: s * 3, height: s * 3), color(0, 0, 0))
  }
  for row in 0..<n { for col in 0..<n {
    let inFinder = (row < 8 && col < 8) || (row < 8 && col >= n - 8) || (row >= n - 8 && col < 8)
    if !inFinder && ri(0, 1) == 1 { fill(ctx, CGRect(x: x0 + s * Double(col), y: y0 + s * Double(row), width: s, height: s), color(0, 0, 0)) }
  } }
  finder(0, 0); finder(n - 7, 0); finder(0, n - 7)
  text(ctx, pick(["Scan to pay", "Scan for the menu", "Scan to check in", "Wi-Fi: scan to join"]), x0, y0 - 40, size: 18, font: "Helvetica-Bold")
}

func barcodeAt(_ ctx: CGContext, _ x0: Double, _ y0: Double, _ w: Double, _ h: Double) {
  var x = x0
  while x < x0 + w { let bw = Double(ri(1, 4)) * 1.3; if ri(0, 1) == 1 { fill(ctx, CGRect(x: x, y: y0, width: bw, height: h), color(0, 0, 0)) }; x += bw }
  text(ctx, (0..<12).map { _ in String(ri(0, 9)) }.joined(), x0 + w / 4, y0 - 14, size: 11, font: "Courier")
}
let barcode: Draw = { ctx, b in
  stroke(ctx, CGRect(x: 80, y: 120, width: b.width - 160, height: b.height - 240), color(0, 0, 0), 2)
  text(ctx, "SHIP TO:", 100, b.height - 160, size: 12, font: "Helvetica-Bold")
  text(ctx, "\(ri(1, 200)) \(pick(["Elm", "Oak", "Mill", "Bridge"])) Street, \(pick(["Leeds", "Bristol", "York", "Bath"]))", 100, b.height - 180, size: 12)
  barcodeAt(ctx, 110, b.height / 2, b.width - 220, r(110, 160))
  barcodeAt(ctx, 110, 170, b.width - 300, 60)
  text(ctx, "TRACKING", 100, b.height / 2 - 40, size: 10)
}

let spreadsheet: Draw = { ctx, b in
  let cols = ri(6, 8), rows = ri(25, 32), cw = (b.width - 100) / Double(cols), ch = (b.height - 160) / Double(rows)
  for row in 0..<rows { for col in 0..<cols {
    let rect = CGRect(x: 50 + cw * Double(col), y: b.height - 80 - ch * Double(row + 1), width: cw, height: ch)
    if row == 0 { fill(ctx, rect, color(0.82, 0.88, 0.95)) } else if row % 2 == 0 { fill(ctx, rect, color(0.97, 0.97, 0.97)) }
    stroke(ctx, rect, color(0.7, 0.7, 0.7), 0.5)
    let t = row == 0 ? ["Item", "Qty", "Price", "Total", "Date", "Code", "Region", "Owner"][col % 8]
      : col == 0 ? pick(words) : String(format: "%.2f", r(1, 999))
    text(ctx, t, rect.minX + 3, rect.minY + 3, size: min(8, ch * 0.6), font: row == 0 ? "Helvetica-Bold" : "Helvetica")
  } }
}

let certificate: Draw = { ctx, b in
  let c = pick([color(0.6, 0.45, 0.1), color(0.1, 0.25, 0.5), color(0.4, 0.1, 0.15)])
  stroke(ctx, b.insetBy(dx: 20, dy: 20), c, 8); stroke(ctx, b.insetBy(dx: 36, dy: 36), c, 2)
  text(ctx, "CERTIFICATE", b.width / 2 - 170, b.height - 140, size: 44, font: "Times-Bold", color: c)
  text(ctx, "OF \(pick(["ACHIEVEMENT", "COMPLETION", "EXCELLENCE", "APPRECIATION"]))", b.width / 2 - 120, b.height - 180, size: 22, font: "Times-Roman", color: c)
  text(ctx, "This is presented to", b.width / 2 - 70, b.height / 2 + 20, size: 14, font: "Times-Italic")
  text(ctx, pick(["Alex Morgan", "Sam Taylor", "Jordan Lee", "Casey Brown"]), b.width / 2 - 120, b.height / 2 - 30, size: 36, font: "Snell Roundhand")
  line(ctx, [(b.width / 2 - 160, b.height / 2 - 40), (b.width / 2 + 160, b.height / 2 - 40)], c, 1)
  ctx.setFillColor(color(0.85, 0.65, 0.15)); ctx.fillEllipse(in: CGRect(x: b.width - 190, y: 60, width: 90, height: 90))
  line(ctx, [(90, 100), (260, 100)], color(0, 0, 0), 1); text(ctx, "Director", 140, 84, size: 10, font: "Times-Roman")
}

let sudoku: Draw = { ctx, b in
  let s = r(42, 52), x0 = (b.width - s * 9) / 2, y0 = (b.height - s * 9) / 2
  for i in 0...9 {
    let w = i % 3 == 0 ? 3.0 : 0.8
    line(ctx, [(x0 + s * Double(i), y0), (x0 + s * Double(i), y0 + s * 9)], color(0, 0, 0), w)
    line(ctx, [(x0, y0 + s * Double(i)), (x0 + s * 9, y0 + s * Double(i))], color(0, 0, 0), w)
  }
  for row in 0..<9 { for col in 0..<9 where ri(0, 2) == 0 {
    text(ctx, "\(ri(1, 9))", x0 + s * Double(col) + s * 0.33, y0 + s * Double(row) + s * 0.28, size: s * 0.55)
  } }
  text(ctx, "Puzzle \(ri(1, 200))  \u{2022}  \(pick(["Easy", "Medium", "Hard"]))", x0, y0 + s * 9 + 24, size: 14, font: "Helvetica-Bold")
}

func resistor(_ ctx: CGContext, _ x: Double, _ y: Double, vertical: Bool) {
  var pts: [(Double, Double)] = []
  for i in 0...8 { let o = i == 0 || i == 8 ? 0.0 : (i % 2 == 0 ? -8.0 : 8.0); pts.append(vertical ? (x + o, y + Double(i) * 6) : (x + Double(i) * 6, y + o)) }
  line(ctx, pts, color(0, 0, 0), 1.5)
}
let circuit: Draw = { ctx, b in
  let x0 = 120.0, y0 = 220.0, w = b.width - 240, h = 360.0
  line(ctx, [(x0, y0), (x0, y0 + h * 0.4)], color(0, 0, 0), 1.5)
  line(ctx, [(x0 - 18, y0 + h * 0.4), (x0 + 18, y0 + h * 0.4)], color(0, 0, 0), 2.5)  // battery
  line(ctx, [(x0 - 9, y0 + h * 0.4 + 10), (x0 + 9, y0 + h * 0.4 + 10)], color(0, 0, 0), 2.5)
  line(ctx, [(x0, y0 + h * 0.4 + 10), (x0, y0 + h), (x0 + w * 0.3, y0 + h)], color(0, 0, 0), 1.5)
  resistor(ctx, x0 + w * 0.3, y0 + h, vertical: false)
  line(ctx, [(x0 + w * 0.3 + 48, y0 + h), (x0 + w, y0 + h), (x0 + w, y0 + h * 0.6)], color(0, 0, 0), 1.5)
  resistor(ctx, x0 + w, y0 + h * 0.6 - 48, vertical: true)
  line(ctx, [(x0 + w, y0 + h * 0.6 - 48), (x0 + w, y0), (x0 + w * 0.55, y0)], color(0, 0, 0), 1.5)
  line(ctx, [(x0 + w * 0.55, y0 - 18), (x0 + w * 0.55, y0 + 18)], color(0, 0, 0), 2)  // capacitor
  line(ctx, [(x0 + w * 0.55 - 10, y0 - 18), (x0 + w * 0.55 - 10, y0 + 18)], color(0, 0, 0), 2)
  line(ctx, [(x0 + w * 0.55 - 10, y0), (x0, y0)], color(0, 0, 0), 1.5)
  line(ctx, [(x0 + w * 0.6, y0 + h), (x0 + w * 0.6, y0 + h * 0.7)], color(0, 0, 0), 1.5)
  resistor(ctx, x0 + w * 0.6, y0 + h * 0.7 - 48, vertical: true)
  line(ctx, [(x0 + w * 0.6, y0 + h * 0.7 - 48), (x0 + w * 0.6, y0 + h * 0.3)], color(0, 0, 0), 1.5)
  for (i, l) in ["R1 \(ri(1, 99))k", "R2 \(ri(1, 99))k", "C1 \(ri(1, 470))uF", "9V"].enumerated() {
    text(ctx, l, [x0 + w * 0.3, x0 + w + 14, x0 + w * 0.5, x0 - 50][i], [y0 + h + 14, y0 + h * 0.6 - 20, y0 - 32, y0 + h * 0.4][i], size: 10)
  }
  for gx in 0..<3 { line(ctx, [(x0 + w * 0.6 - 12 + Double(gx) * 4, y0 + h * 0.3 - Double(gx) * 6), (x0 + w * 0.6 + 12 - Double(gx) * 4, y0 + h * 0.3 - Double(gx) * 6)], color(0, 0, 0), 1.5) }
}

var photos: [URL] = (try? FileManager.default.contentsOfDirectory(at: photoDir, includingPropertiesForKeys: nil))?
  .filter { $0.pathExtension.lowercased() == "jpg" }.sorted { $0.path < $1.path } ?? []
let photo: Draw = { ctx, b in
  guard !photos.isEmpty, let src = CGImageSourceCreateWithURL(photos.removeFirst() as CFURL, nil),
        let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return }
  // A full-page picture (a scanned print): an image over most of the page.
  let w = b.width - 60, h = w * Double(img.height) / Double(img.width)
  ctx.draw(img, in: CGRect(x: 30, y: max(30, (b.height - h) / 2), width: w, height: min(h, b.height - 60)))
}

let businessCard: Draw = { ctx, b in
  let c = pick(palette)
  fill(ctx, CGRect(x: 0, y: 0, width: 10, height: b.height), c)
  text(ctx, pick(["Alex Morgan", "Sam Taylor", "Jordan Lee", "Casey Brown", "Riley Quinn"]), 24, b.height - 40, size: 16, font: "Helvetica-Bold")
  text(ctx, pick(["Sales Director", "Architect", "Software Engineer", "Consultant"]), 24, b.height - 58, size: 9, color: color(0.4, 0.4, 0.4))
  text(ctx, "+44 \(ri(1000, 9999)) \(ri(100000, 999999))", 24, 40, size: 8)
  text(ctx, "\(pick(["alex", "sam", "jo", "casey"]))@\(pick(["acme", "northwind", "globex"])).com", 24, 28, size: 8)
  ctx.setFillColor(c); ctx.fillEllipse(in: CGRect(x: b.width - 60, y: b.height - 60, width: 36, height: 36))
}

let kinds: [(String, Draw, CGSize?)] = [
  ("receipt", receipt, nil), ("floor plan", floorPlan, nil), ("bar chart", barChart, nil), ("pie chart", pieChart, nil),
  ("line graph", lineGraph, nil), ("slide", slide, CGSize(width: 960, height: 540)), ("sheet music", sheetMusic, nil),
  ("calendar", calendar, nil), ("crossword", crossword, nil), ("chessboard", chessboard, nil), ("flowchart", flowchart, nil),
  ("map", map, nil), ("qr code", qrCode, nil), ("barcode", barcode, nil), ("spreadsheet", spreadsheet, nil),
  ("certificate", certificate, CGSize(width: 792, height: 612)), ("sudoku", sudoku, nil), ("circuit", circuit, nil),
  ("photo", photo, nil), ("business card", businessCard, CGSize(width: 252, height: 144)),
]

var labels = "file\tkind\tpage\tpages\n"
var serial = 0
for (name, draw, size) in kinds {
  for i in 0..<6 {
    serial += 1
    let file = String(format: "doc_v%03d.pdf", ri(100, 999) * 1000 + serial)
    // Where the visual page sits: page 0, 1 or 2 (the first pages are always
    // pictured); a photo also deep in a long document (page 4..8), found only
    // by the "mostly a picture" rule. A business card is one card.
    let pages = size?.width == 252 ? 1 : (name == "photo" && i >= 2 ? ri(9, 12) : ri(1, 6))
    let at = pages == 1 ? 0 : name == "photo" && i >= 2 ? ri(4, 8) : min(pages - 1, i % 3)
    var box = CGRect(origin: .zero, size: size ?? CGSize(width: 612, height: 792))
    let url = out.appendingPathComponent(file)
    guard let ctx = CGContext(url as CFURL, mediaBox: &box, nil) else { continue }
    for p in 0..<pages {
      if p == at {
        let pb = CGRect(origin: .zero, size: size ?? CGSize(width: 612, height: 792))
        var mb = pb
        ctx.beginPage(mediaBox: &mb)
        fill(ctx, pb, color(1, 1, 1))
        draw(ctx, pb)
      } else {
        var mb = CGRect(x: 0, y: 0, width: 612, height: 792)
        ctx.beginPage(mediaBox: &mb)
        prose(ctx, mb)
      }
      ctx.endPage()
    }
    ctx.closePDF()
    labels += "\(file)\t\(name)\t\(at)\t\(pages)\n"
  }
}
try labels.write(to: out.appendingPathComponent("labels.tsv"), atomically: true, encoding: .utf8)
print("wrote \(serial) documents")
