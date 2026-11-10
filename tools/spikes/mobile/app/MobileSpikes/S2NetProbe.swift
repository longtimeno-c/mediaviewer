// M1 spike S2 (throwaway): the range server through plain sockets, for the NAT64 test. Logs address
// families only, never an address: this report goes in the repository.
import Darwin
import Foundation

enum S2NetProbe {
  private static func describe(_ a: mv_net_attempt) -> [String: Any] {
    [
      "family": Int(a.family),
      "well_known_nat64": a.well_known_nat64 != 0,
      "connected": a.connected != 0,
      "error": a.error == 0 ? "" : String(cString: strerror(a.error)),
      "connect_ms": Measure.rounded(a.connect_ms),
      "first_byte_ms": Measure.rounded(a.first_byte_ms),
      "total_ms": Measure.rounded(a.total_ms),
      "http_status": Int(a.http_status),
      "body_bytes": a.body_bytes,
    ]
  }

  private static func succeeded(_ a: mv_net_attempt) -> Bool {
    a.connected != 0 && a.error == 0 && a.http_status == 206
  }

  static func run(url text: String, trigger: String) {
    let report = Report.shared
    guard let url = URL(string: text), let host = url.host else {
      report.emit("s2net", ["error": "no range server URL"])
      return
    }
    let port = String(url.port ?? 80)
    // Still escaped: decoded, a clip name with a space would break the request line.
    let escaped = url.path(percentEncoded: true)
    let path = escaped.isEmpty ? "/" : escaped
    let rangeBytes = 64 * 1024
    let literal = mv_net_is_ipv4_literal(host) != 0
    let dns64 = mv_net_has_dns64() != 0

    report.enter("s2net.getaddrinfo")
    var attempts = [mv_net_attempt](repeating: mv_net_attempt(), count: 8)
    var resolveMs = 0.0
    var gaiError: Int32 = 0
    let used = mv_net_probe_name(host, port, path, rangeBytes, &attempts, attempts.count, &resolveMs, &gaiError)
    let tried = Array(attempts.prefix(used))
    report.leave()
    report.emit("s2net", [
      "trigger": trigger,
      "mode": "getaddrinfo",
      "host_kind": literal ? "ipv4_literal" : "name",
      "dns64": dns64,
      "resolve_ms": Measure.rounded(resolveMs),
      "gai_error": gaiError == 0 ? "" : String(cString: gai_strerror(gaiError)),
      "ok": tried.contains(where: succeeded),
      "answered_family": tried.first(where: succeeded).map { Int($0.family) } ?? 0,
      "attempts": tried.map(describe),
    ])

    guard literal else { return }
    report.enter("s2net.raw_ipv4")
    var raw = mv_net_attempt()
    _ = mv_net_probe_raw_ipv4(host, port, path, rangeBytes, &raw)
    report.leave()
    report.emit("s2net", [
      "trigger": trigger,
      "mode": "raw_ipv4",
      "host_kind": "ipv4_literal",
      "dns64": dns64,
      "ok": succeeded(raw),
      "answered_family": succeeded(raw) ? 4 : 0,
      "attempts": [describe(raw)],
    ])
  }
}
