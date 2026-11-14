// M1 spikes (throwaway). What every spike reports, and where it goes.
import Darwin
import Foundation
import UIKit
import os

/// Clocks for a cold start. iOS can prewarm a process long before anyone taps, which inflates any
/// clock taken from the process start, so `didFinishMs` is the one to trust.
enum Launch {
  /// Touched first thing in App.init.
  static let mainMs = Measure.nowMs()
  static let processAgeAtMainMs = Measure.processAgeMs()
  static var didFinishMs: Double?
  static var prewarmed: Bool { ProcessInfo.processInfo.environment["ActivePrewarm"] == "1" }
}

enum Measure {
  static func nowMs() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1_000_000 }

  /// The number jetsam judges the app by, and its high-water mark, in MB.
  static func footprintMB() -> (now: Double, peak: Double) {
    var info = task_vm_info_data_t()
    var count = mach_msg_type_number_t(
      MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
    let kr = withUnsafeMutablePointer(to: &info) { p in
      p.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
        task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
      }
    }
    guard kr == KERN_SUCCESS else { return (-1, -1) }
    return (Double(info.phys_footprint) / 1_048_576, Double(info.ledger_phys_footprint_peak) / 1_048_576)
  }

  /// How much more the app may take before jetsam. Reports 0 on the simulator, which has no limit.
  static func availableMB() -> Double { Double(os_proc_available_memory()) / 1_048_576 }

  /// Milliseconds since the kernel started this process: the cold-start clock.
  static func processAgeMs() -> Double {
    var kp = kinfo_proc()
    var size = MemoryLayout<kinfo_proc>.stride
    var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
    guard sysctl(&mib, 4, &kp, &size, nil, 0) == 0 else { return -1 }
    let start = kp.kp_proc.p_starttime
    let startS = Double(start.tv_sec) + Double(start.tv_usec) / 1_000_000
    return (Date().timeIntervalSince1970 - startS) * 1000
  }

  static func sysctlInt(_ name: String) -> Int {
    var value: Int32 = 0
    var size = MemoryLayout<Int32>.size
    return sysctlbyname(name, &value, &size, nil, 0) == 0 ? Int(value) : -1
  }

  static func machine() -> String {
    if let sim = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] { return sim }
    var size = 0
    sysctlbyname("hw.machine", nil, &size, nil, 0)
    var bytes = [CChar](repeating: 0, count: max(size, 1))
    sysctlbyname("hw.machine", &bytes, &size, nil, 0)
    return String(cString: bytes)
  }

  static var isSimulator: Bool {
    #if targetEnvironment(simulator)
      return true
    #else
      return false
    #endif
  }

  static func performanceCores() -> Int {
    let n = sysctlInt("hw.perflevel0.physicalcpu")
    return n > 0 ? n : max(1, ProcessInfo.processInfo.activeProcessorCount / 2)
  }

  static func thermal() -> String {
    switch ProcessInfo.processInfo.thermalState {
    case .nominal: return "nominal"
    case .fair: return "fair"
    case .serious: return "serious"
    case .critical: return "critical"
    @unknown default: return "unknown"
    }
  }

  static func percentile(_ values: [Double], _ p: Double) -> Double {
    guard !values.isEmpty else { return -1 }
    let sorted = values.sorted()
    let at = min(sorted.count - 1, Int((Double(sorted.count - 1) * p).rounded()))
    return sorted[at]
  }

  static func rounded(_ v: Double) -> Double { (v * 100).rounded() / 100 }
}

/// One JSON line per result, in Documents/results.jsonl and on stdout.
final class Report: ObservableObject {
  static let shared = Report()
  @Published var lines: [String] = []
  private let queue = DispatchQueue(label: "m1.report")
  private var memoryWarnings = 0

  static var documents: URL {
    FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
  }
  private var resultsURL: URL { Self.documents.appendingPathComponent("results.jsonl") }
  private var breadcrumbURL: URL { Self.documents.appendingPathComponent("breadcrumb.txt") }

  init() {
    NotificationCenter.default.addObserver(
      forName: UIApplication.didReceiveMemoryWarningNotification, object: nil, queue: nil
    ) { [weak self] _ in
      self?.queue.async { self?.memoryWarnings += 1 }
    }
  }

  /// Say a step is starting, before it can be killed. A launch that finds this file reports that
  /// the last run died there: that is the jetsam signal, since jetsam leaves the app no last word.
  func enter(_ step: String) {
    try? Data(step.utf8).write(to: breadcrumbURL, options: .atomic)
  }

  func leave() { try? FileManager.default.removeItem(at: breadcrumbURL) }

  func reportKilledRunIfAny() {
    guard let data = try? Data(contentsOf: breadcrumbURL),
      let step = String(data: data, encoding: .utf8)
    else { return }
    leave()
    emit("killed", ["during": step, "meaning": "the previous run ended inside this step (jetsam, a crash or a force quit)"])
  }

  func emit(_ spike: String, _ fields: [String: Any]) {
    let mem = Measure.footprintMB()
    var all = fields
    all["spike"] = spike
    all["device"] = Measure.machine()
    all["ios"] = UIDevice.current.systemVersion
    all["simulator"] = Measure.isSimulator
    all["prewarmed"] = Launch.prewarmed
    all["thermal"] = Measure.thermal()
    all["low_power"] = ProcessInfo.processInfo.isLowPowerModeEnabled
    all["uptime_s"] = Int(ProcessInfo.processInfo.systemUptime)
    all["footprint_mb"] = Measure.rounded(mem.now)
    all["footprint_peak_mb"] = Measure.rounded(mem.peak)
    all["available_mb"] = Measure.rounded(Measure.availableMB())
    all["at"] = ISO8601DateFormatter().string(from: Date())
    queue.async {
      all["memory_warnings"] = self.memoryWarnings
      guard
        let json = try? JSONSerialization.data(withJSONObject: all, options: [.sortedKeys]),
        let line = String(data: json, encoding: .utf8)
      else { return }
      print("MVSPIKE \(line)")
      fflush(stdout)
      if let handle = try? FileHandle(forWritingTo: self.resultsURL) {
        handle.seekToEndOfFile()
        handle.write(Data((line + "\n").utf8))
        try? handle.close()
      } else {
        try? Data((line + "\n").utf8).write(to: self.resultsURL)
      }
      DispatchQueue.main.async { self.lines.insert(line, at: 0) }
    }
  }

  /// Waits for everything emitted so far to be on disk.
  func flush() { queue.sync {} }
}
