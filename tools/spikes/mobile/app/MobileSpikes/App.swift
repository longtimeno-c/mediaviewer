// M1 spikes (throwaway). Buttons for the owner; `--run` for a script. README.md says what to press.
import AVFoundation
import SwiftUI

@main
struct MobileSpikesApp: App {
  @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

  init() {
    _ = Launch.mainMs
    _ = Launch.processAgeAtMainMs
  }

  var body: some Scene {
    WindowGroup { ContentView() }
  }
}

final class AppDelegate: NSObject, UIApplicationDelegate {
  func application(_ application: UIApplication,
                   didFinishLaunchingWithOptions options: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
    Launch.didFinishMs = Measure.nowMs()
    return true
  }
}

/// `--run s1,s3:100000:heap,s4:file --exit`. A launch argument rather than a tap, so a cold start
/// is measured from the moment the process exists.
enum Script {
  static var steps: [String] {
    let args = ProcessInfo.processInfo.arguments
    guard let at = args.firstIndex(of: "--run"), at + 1 < args.count else { return [] }
    return args[at + 1].split(separator: ",").map(String.init)
  }
  static var exits: Bool { ProcessInfo.processInfo.arguments.contains("--exit") }
  static var httpURL: String? {
    let args = ProcessInfo.processInfo.arguments
    guard let at = args.firstIndex(of: "--http"), at + 1 < args.count else { return nil }
    return args[at + 1]
  }
}

struct ContentView: View {
  @StateObject private var report = Report.shared
  @StateObject private var s4 = S4Run()
  @AppStorage("autorunS1") private var autorunS1 = false
  @AppStorage("httpURL") private var httpURL = ""
  @State private var busy = false
  @State private var started = false

  var body: some View {
    NavigationStack {
      // Above the form, always on screen: a Form builds rows lazily, and a layer that is not in a
      // window never reports its first frame. A new layer per run, or its signal fires only once.
      if let player = s4.player {
        PlayerLayerView(player: player) { s4.layerReady = true }
          .frame(height: 200)
          .id(ObjectIdentifier(player))
      }
      Form {
        Section("S1 CLIP text tower") {
          Button("Run (performance cores)") { background { S1Clip.run(threads: 0, repeats: 10, trigger: "button") } }
          Button("Run (1 thread)") { background { S1Clip.run(threads: 1, repeats: 10, trigger: "button") } }
          Toggle("Run at launch (cold start)", isOn: $autorunS1)
        }
        Section("S3 scan") {
          Button("100k, heap") { background { S3Scan.run(rows: 100_000, mode: "heap", passes: 7, trigger: "button") } }
          Button("100k, mapped file") { background { S3Scan.run(rows: 100_000, mode: "mmap", passes: 7, trigger: "button") } }
          Button("1M, mapped file") { background { S3Scan.run(rows: 1_000_000, mode: "mmap", passes: 7, trigger: "button") } }
          Button("1M, heap (may be killed)") { background { S3Scan.run(rows: 1_000_000, mode: "heap", passes: 7, trigger: "button") } }
          Button("Delete S3 files", role: .destructive) { background { S3Scan.deleteCacheFiles() } }
        }
        Section("S4 AVPlayer over a resource loader") {
          TextField("http://host:port/token/clip.mp4", text: $httpURL)
            .textInputAutocapitalization(.never).autocorrectionDisabled().keyboardType(.URL)
          Button("Play and seek: clip in Documents") { runS4("file") }
          Button("Play and seek: range server") { runS4("http") }
          Button("Plain socket probe: range server (NAT64)") {
            let url = httpURL
            background { S2NetProbe.run(url: url, trigger: "button") }
          }
        }
        Section("Results, newest first") {
          if busy { ProgressView() }
          ForEach(report.lines.prefix(30), id: \.self) { line in
            Text(line).font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
          }
        }
      }
      .navigationTitle("M1 spikes")
      .disabled(busy)
    }
    .task { await launch() }
  }

  private func background(_ work: @escaping () -> Void) {
    busy = true
    DispatchQueue.global(qos: .userInitiated).async {
      work()
      DispatchQueue.main.async { busy = false }
    }
  }

  private func source(_ kind: String) throws -> ByteSource {
    if kind == "http" {
      guard let url = URL(string: Script.httpURL ?? httpURL), url.scheme != nil else {
        throw NSError(domain: "S4", code: 1, userInfo: [NSLocalizedDescriptionKey: "no range server URL"])
      }
      return HTTPSource(url: url)
    }
    guard let clip = S4Run.localClip() else {
      throw NSError(domain: "S4", code: 2, userInfo: [NSLocalizedDescriptionKey: "no .mp4 or .mov in Documents (README, S4)"])
    }
    return try FileSource(url: clip)
  }

  private func playAndSeek(_ kind: String, trigger: String) async {
    do {
      try AVAudioSession.sharedInstance().setCategory(.playback)
      await s4.run(source: try source(kind), trigger: trigger)
    } catch {
      Report.shared.emit("s4", ["source": kind, "error": "\(error)"])
    }
  }

  private func runS4(_ kind: String) {
    busy = true
    Task {
      await playAndSeek(kind, trigger: "button")
      busy = false
    }
  }

  private func launch() async {
    guard !started else { return }
    started = true
    Report.shared.reportKilledRunIfAny()
    var steps = Script.steps
    if steps.isEmpty && autorunS1 { steps = ["s1"] }
    guard !steps.isEmpty else { return }
    busy = true
    for step in steps {
      let parts = step.split(separator: ":").map(String.init)
      switch parts[0] {
      case "s1":
        let threads = parts.count > 1 ? Int(parts[1]) ?? 0 : 0
        await offMain { S1Clip.run(threads: threads, repeats: 10, trigger: "launch") }
      case "s3":
        let rows = parts.count > 1 ? Int(parts[1]) ?? 100_000 : 100_000
        let mode = parts.count > 2 ? parts[2] : "heap"
        await offMain { S3Scan.run(rows: rows, mode: mode, passes: 7, trigger: "launch") }
      case "s2net":
        let url = Script.httpURL ?? httpURL
        await offMain { S2NetProbe.run(url: url, trigger: "launch") }
      case "s4":
        await playAndSeek(parts.count > 1 ? parts[1] : "file", trigger: "launch")
      default:
        Report.shared.emit("script", ["error": "unknown step \(step)"])
      }
    }
    busy = false
    Report.shared.flush()
    if Script.exits { exit(0) }
  }

  private func offMain(_ work: @escaping () -> Void) async {
    await withCheckedContinuation { c in
      DispatchQueue.global(qos: .userInitiated).async {
        work()
        c.resume()
      }
    }
  }
}
