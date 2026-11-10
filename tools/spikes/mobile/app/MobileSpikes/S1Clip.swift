// M1 spike S1 (throwaway): the CLIP text tower with ONNX Runtime on the CPU.
//
// Through ort_bridge: the C API, linked statically. None of src/infer or src/addons/ai.
// BLOCKERS.md says what the real port needs.
import Foundation

enum S1Clip {
  static let contextLength = 77
  static let endToken: Int64 = 49407
  static let optimizationAll: Int32 = 99

  struct Query: Decodable {
    let text: String
    let ids: [Int64]
  }

  static func modelURL() -> URL { Report.documents.appendingPathComponent("text.onnx") }

  static func loadQueries() throws -> [Query] {
    guard let url = Bundle.main.url(forResource: "queries", withExtension: "json") else {
      throw NSError(domain: "S1", code: 1, userInfo: [NSLocalizedDescriptionKey: "queries.json is not in the app"])
    }
    return try JSONDecoder().decode([Query].self, from: Data(contentsOf: url))
  }

  /// One whole run in a fresh session, so every number includes a load. The footprint's high-water
  /// mark belongs to the process: run S1 first after a launch for a clean peak.
  static func run(threads requested: Int, repeats: Int, trigger: String) {
    let report = Report.shared
    let path = modelURL().path
    guard FileManager.default.fileExists(atPath: path) else {
      report.emit("s1", ["error": "no text.onnx in Documents (README, S1)"])
      return
    }
    let threads = requested > 0 ? requested : Measure.performanceCores()
    var errorBuffer = [CChar](repeating: 0, count: 1024)
    do {
      let queries = try loadQueries()
      let modelMB = Double((try FileManager.default.attributesOfItem(atPath: path)[.size] as? Int64) ?? 0) / 1_048_576
      let ageAtStart = Measure.processAgeMs()
      let before = Measure.footprintMB()

      report.enter("s1.load threads=\(threads)")
      let t0 = Measure.nowMs()
      guard let ort = mv_ort_open(path, Int32(threads), optimizationAll, &errorBuffer, errorBuffer.count) else {
        report.leave()
        report.emit("s1", ["error": String(cString: errorBuffer), "ort": String(cString: mv_ort_version())])
        return
      }
      defer { mv_ort_close(ort) }
      let loadMs = Measure.nowMs() - t0
      let afterLoad = Measure.footprintMB()
      let availableAfterLoad = Measure.availableMB()

      var embedding = [Float](repeating: 0, count: 2048)
      func encode(_ q: Query) -> (ms: Double, dim: Int, norm: Double, error: String?) {
        var ids = Array(q.ids.prefix(contextLength))
        let used = ids.count
        ids += Array(repeating: endToken, count: contextLength - used)
        let start = Measure.nowMs()
        let n = mv_ort_encode(ort, ids, contextLength, used, &embedding, embedding.count, &errorBuffer, errorBuffer.count)
        let ms = Measure.nowMs() - start
        if n < 0 { return (ms, 0, 0, String(cString: errorBuffer)) }
        let norm = embedding.prefix(n).reduce(0.0) { $0 + Double($1) * Double($1) }.squareRoot()
        return (ms, n, norm, nil)
      }

      report.enter("s1.first_encode threads=\(threads)")
      let first = encode(queries[0])
      if let message = first.error {
        report.leave()
        report.emit("s1", ["error": message, "ort": String(cString: mv_ort_version())])
        return
      }
      let launchToEncoded = Measure.processAgeMs()
      let encodedAt = Measure.nowMs()
      let afterFirst = Measure.footprintMB()

      report.enter("s1.encodes threads=\(threads)")
      var times: [Double] = []
      for i in 0..<(repeats * queries.count) { times.append(encode(queries[i % queries.count]).ms) }
      let afterAll = Measure.footprintMB()
      report.leave()

      report.emit("s1", [
        "trigger": trigger,
        "ort": String(cString: mv_ort_version()),
        "threads": threads,
        "model_mb": Measure.rounded(modelMB),
        "inputs": String(cString: mv_ort_inputs(ort)),
        "output": String(cString: mv_ort_output(ort)),
        "embedding_dim": first.dim,
        "embedding_norm": Measure.rounded(first.norm),
        "process_age_at_start_ms": Measure.rounded(ageAtStart),
        "load_ms": Measure.rounded(loadMs),
        "first_encode_ms": Measure.rounded(first.ms),
        "process_start_to_first_encode_ms": Measure.rounded(launchToEncoded),
        "process_age_at_main_ms": Measure.rounded(Launch.processAgeAtMainMs),
        "main_to_first_encode_ms": Measure.rounded(encodedAt - Launch.mainMs),
        "did_finish_to_first_encode_ms": Launch.didFinishMs.map { Measure.rounded(encodedAt - $0) } ?? -1,
        "available_mb_after_load": Measure.rounded(availableAfterLoad),
        "encode_count": times.count,
        "encode_p50_ms": Measure.rounded(Measure.percentile(times, 0.5)),
        "encode_p95_ms": Measure.rounded(Measure.percentile(times, 0.95)),
        "encode_max_ms": Measure.rounded(times.max() ?? -1),
        "mb_before": Measure.rounded(before.now),
        "mb_after_load": Measure.rounded(afterLoad.now),
        "mb_peak_after_load": Measure.rounded(afterLoad.peak),
        "mb_after_first_encode": Measure.rounded(afterFirst.now),
        "mb_after_all": Measure.rounded(afterAll.now),
      ])
    } catch {
      report.leave()
      report.emit("s1", ["error": "\(error)"])
    }
  }
}
