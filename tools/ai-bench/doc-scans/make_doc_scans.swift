// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #354 D4 verify set: scanned documents, PDFs whose pages are pictures of text
// with no text layer. Each page is typeset with CoreText (letters, invoices,
// reports: a heading, a date, body paragraphs in one of five fonts at 9-12 pt
// and a small-print footer at 7.5 pt), rasterised at 200 dpi in grey, turned by
// up to a degree, given paper tone and sensor noise, JPEG-compressed, and
// drawn full-page into a PDF: what a flatbed or a phone scanner app writes.
//
//   swift make_doc_scans.swift <out dir> [--photos <dir> <count>]
//
// Writes <out>/scans/doc_s001.pdf ... doc_s050.pdf (one page each),
// <out>/book/book_scan.pdf (100 pages), truth.tsv (file, page, the words drawn)
// and queries.txt (every scored word once, quoted, and a 4-word phrase from
// each book page). --photos also draws <count> camera-sized JPEGs (4032 x 3024)
// for the "never holds up the photo pass" run. Then: ai-bench --documents
// --brief --top 200 --query-file queries.txt, and score_doc_scans.py.
import AppKit
import CoreText
import Foundation
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
let out = URL(fileURLWithPath: args[1])
var photoDir: URL? = nil
var photoCount = 0
if let i = args.firstIndex(of: "--photos"), i + 2 < args.count {
  photoDir = URL(fileURLWithPath: args[i + 1])
  photoCount = Int(args[i + 2]) ?? 0
}
let fm = FileManager.default
for d in ["scans", "book"] { try? fm.createDirectory(at: out.appendingPathComponent(d), withIntermediateDirectories: true) }

struct Rng: RandomNumberGenerator {
  var s: UInt64
  mutating func next() -> UInt64 { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s }
}
var rng = Rng(s: 0x5CA7_D4D4)
func r(_ a: Double, _ b: Double) -> Double { Double.random(in: a...b, using: &rng) }
func ri(_ a: Int, _ b: Int) -> Int { Int.random(in: a...b, using: &rng) }
func pick<T>(_ xs: [T]) -> T { xs.randomElement(using: &rng)! }

// Everyday words of letters, invoices, reports and manuals.
let vocab = """
account address agreement amount annual answer appointment approval april arrival article assessment \
assistance attached august authority available balance bank basis battery behalf benefit bill board \
booking branch budget building business calendar camera cancel capital card careful carpet case cash \
certificate change charge check claim client closing collection committee company complaint condition \
confirm contact contract copy corner council country course cover credit current customer damage date \
deadline december decision delivery deposit description design detail development device director \
discount district document door draft driver duty early electric employee energy engine enquiry entry \
equipment estate estimate evening event example expense experience extension family february fee \
figure file final finance floor folder form forward friday fuel full fund furniture garage garden \
general gift glass government grant ground group guarantee guest hall health heating holiday home \
hospital hotel hour house income increase information insurance interest invoice issue item january \
journey july june kitchen label land late lease letter level licence light limit list loan local \
machine maintenance manager manual march market material meeting member method mileage minutes monday \
month morning mortgage notice november number october office officer opening order owner package page \
paper parking part payment pension period permit person phone place plan planning policy post premium \
price print procedure process product programme project property proposal purchase quality quantity \
quarter question range rate reading receipt record reference refund region register reminder rent \
repair report request reserve resident response result return review road room route safety salary \
sale saturday schedule school season second section security service session settlement share \
shipment signature site size statement station status stock storage street student summary sunday \
supplier supply support surface survey system table tax telephone tenant term thursday ticket total \
tower town trade training transfer travel treatment tuesday unit upgrade usage valid value vehicle \
village visit volume wall warranty water wednesday week weight window winter work workshop year
""".split(separator: " ").map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
let joiners = ["the", "of", "and", "for", "to", "with", "on", "in", "by", "from", "at", "is", "was", "will be", "has"]

func sentence() -> String {
  var w: [String] = []
  for i in 0..<ri(7, 15) {
    if i > 0 && i % 3 == 0 { w.append(pick(joiners)) }
    w.append(ri(0, 12) == 0 ? String(ri(100, 99999)) : pick(vocab))
  }
  let s = w.joined(separator: " ")
  return s.prefix(1).uppercased() + s.dropFirst() + "."
}
func paragraph() -> String { (0..<ri(3, 6)).map { _ in sentence() }.joined(separator: " ") }
let months = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
              "November", "December"]
let fonts = ["Times New Roman", "Helvetica", "Georgia", "Courier New", "Arial"]

let dpi = 200.0
let pageW = 612.0, pageH = 792.0  // US Letter, points
let pxW = Int(pageW / 72 * dpi), pxH = Int(pageH / 72 * dpi)
let grey = CGColorSpaceCreateDeviceGray()

// Typesets one page; returns its picture and the text drawn.
func typeset(heading: String, footer: String) -> (CGImage, String) {
  let ctx = CGContext(data: nil, width: pxW, height: pxH, bitsPerComponent: 8, bytesPerRow: 0, space: grey,
                      bitmapInfo: CGImageAlphaInfo.none.rawValue)!
  ctx.setFillColor(gray: 1, alpha: 1)
  ctx.fill(CGRect(x: 0, y: 0, width: pxW, height: pxH))
  ctx.scaleBy(x: dpi / 72, y: dpi / 72)
  let face = pick(fonts)
  let size = r(9, 12)
  let date = "\(ri(1, 28)) \(pick(months)) \(ri(2015, 2026))"
  let body = (0..<ri(3, 5)).map { _ in paragraph() }.joined(separator: "\n\n")
  let full = NSMutableAttributedString()
  let bold = CTFontCreateWithName((face + " Bold") as CFString, size + 5, nil)
  full.append(NSAttributedString(string: heading + "\n", attributes: [.font: bold]))
  full.append(NSAttributedString(string: date + "\n\n", attributes: [.font: CTFontCreateWithName(face as CFString, size, nil)]))
  full.append(NSAttributedString(string: body, attributes: [.font: CTFontCreateWithName(face as CFString, size, nil)]))
  let setter = CTFramesetterCreateWithAttributedString(full)
  let box = CGRect(x: 72, y: 96, width: pageW - 144, height: pageH - 168)
  let frame = CTFramesetterCreateFrame(setter, CFRange(location: 0, length: 0), CGPath(rect: box, transform: nil), nil)
  CTFrameDraw(frame, ctx)
  // What fitted: the frame's visible range (a long page is cut at the box).
  let shown = CTFrameGetVisibleStringRange(frame)
  let drawn = (full.string as NSString).substring(with: NSRange(location: shown.location, length: shown.length))
  // Small print.
  let small = NSAttributedString(string: footer, attributes: [.font: CTFontCreateWithName(face as CFString, 7.5, nil)])
  ctx.textPosition = CGPoint(x: 72, y: 54)
  CTLineDraw(CTLineCreateWithAttributedString(small), ctx)
  return (ctx.makeImage()!, drawn + "\n" + footer)
}

// As a scanner leaves it: turned a little, paper tone, noise, JPEG.
func scanned(_ img: CGImage) -> Data {
  let ctx = CGContext(data: nil, width: pxW, height: pxH, bitsPerComponent: 8, bytesPerRow: pxW, space: grey,
                      bitmapInfo: CGImageAlphaInfo.none.rawValue)!
  let tone = r(0.88, 0.97)
  ctx.setFillColor(gray: tone, alpha: 1)
  ctx.fill(CGRect(x: 0, y: 0, width: pxW, height: pxH))
  ctx.translateBy(x: Double(pxW) / 2 + r(-20, 20), y: Double(pxH) / 2 + r(-20, 20))
  ctx.rotate(by: r(-1, 1) * .pi / 180)
  ctx.translateBy(x: -Double(pxW) / 2, y: -Double(pxH) / 2)
  ctx.setBlendMode(.multiply)
  ctx.draw(img, in: CGRect(x: 0, y: 0, width: pxW, height: pxH))
  let p = ctx.data!.bindMemory(to: UInt8.self, capacity: pxW * pxH)
  for i in 0..<(pxW * pxH) {
    let v = Int(p[i]) + Int.random(in: -14...14, using: &rng)
    p[i] = UInt8(max(0, min(255, v)))
  }
  let noisy = ctx.makeImage()!
  let data = NSMutableData()
  let dst = CGImageDestinationCreateWithData(data, UTType.jpeg.identifier as CFString, 1, nil)!
  CGImageDestinationAddImage(dst, noisy, [kCGImageDestinationLossyCompressionQuality: 0.7] as CFDictionary)
  CGImageDestinationFinalize(dst)
  return data as Data
}

func writePDF(_ url: URL, pages: [Data]) {
  var box = CGRect(x: 0, y: 0, width: pageW, height: pageH)
  let pdf = CGContext(url as CFURL, mediaBox: &box, nil)!
  for jpeg in pages {
    let src = CGImageSourceCreateWithData(jpeg as CFData, nil)!
    let img = CGImageSourceCreateImageAtIndex(src, 0, nil)!
    pdf.beginPDFPage(nil)
    pdf.draw(img, in: box)
    pdf.endPDFPage()
  }
  pdf.closePDF()
}

// The words search scores: what the engine's query keeps (letters and digits,
// lower case), stop words left to the scorer.
func tokens(_ s: String) -> [String] {
  s.lowercased().split(whereSeparator: { !$0.isLetter && !$0.isNumber }).map(String.init)
}

var truth = "file\tpage\twords\n"
var queries = Set<String>()
var phraseQueries: [String] = []
let headings = ["Invoice", "Statement of Account", "Tenancy Agreement", "Service Report", "Meeting Minutes",
                "Delivery Note", "Insurance Renewal", "Quarterly Summary", "Warranty Card", "Planning Notice"]
for n in 1...50 {
  let name = String(format: "doc_s%03d.pdf", n)
  let (img, text) = typeset(heading: "\(pick(headings)) \(ri(1000, 9999))",
                            footer: "Registered office \(ri(1, 200)) \(pick(vocab)) street. Reference \(ri(10000, 99999)). Terms and conditions apply.")
  writePDF(out.appendingPathComponent("scans").appendingPathComponent(name), pages: [scanned(img)])
  let words = tokens(text)
  truth += "\(name)\t0\t\(words.joined(separator: " "))\n"
  for w in Set(words) { queries.insert("\"\(w)\"") }
}
var book: [Data] = []
for p in 0..<100 {
  let (img, text) = typeset(heading: "Chapter \(p / 10 + 1)", footer: "Page \(p + 1) of 100")
  book.append(scanned(img))
  let words = tokens(text)
  truth += "book_scan.pdf\t\(p)\t\(words.joined(separator: " "))\n"
  // A phrase from the middle of the page's body: the book is found at this page.
  let mid = words.count / 2
  phraseQueries.append("\"" + words[mid..<min(words.count, mid + 4)].joined(separator: " ") + "\"")
}
writePDF(out.appendingPathComponent("book").appendingPathComponent("book_scan.pdf"), pages: book)
try! truth.write(to: out.appendingPathComponent("truth.tsv"), atomically: true, encoding: .utf8)
try! (queries.sorted() + phraseQueries).joined(separator: "\n").appending("\n")
  .write(to: out.appendingPathComponent("queries.txt"), atomically: true, encoding: .utf8)
print("50 scanned pages, a 100-page scanned book, \(queries.count) words and \(phraseQueries.count) phrases")

if let dir = photoDir, photoCount > 0 {
  try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
  let rgb = CGColorSpaceCreateDeviceRGB()
  for i in 0..<photoCount {
    let w = 4032, h = 3024
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: rgb,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    let g = CGGradient(colorsSpace: rgb, colors: [CGColor(red: r(0, 1), green: r(0, 1), blue: r(0, 1), alpha: 1),
                                                  CGColor(red: r(0, 1), green: r(0, 1), blue: r(0, 1), alpha: 1)] as CFArray,
                       locations: nil)!
    ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: w, y: h), options: [])
    for _ in 0..<40 {
      ctx.setFillColor(CGColor(red: r(0, 1), green: r(0, 1), blue: r(0, 1), alpha: r(0.3, 1)))
      ctx.fillEllipse(in: CGRect(x: r(0, Double(w)), y: r(0, Double(h)), width: r(50, 900), height: r(50, 900)))
    }
    let data = NSMutableData()
    let dst = CGImageDestinationCreateWithData(data, UTType.jpeg.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dst, ctx.makeImage()!, [kCGImageDestinationLossyCompressionQuality: 0.85] as CFDictionary)
    CGImageDestinationFinalize(dst)
    try! (data as Data).write(to: dir.appendingPathComponent(String(format: "IMG_%04d.JPG", i + 1)))
  }
  print("\(photoCount) photos")
}
