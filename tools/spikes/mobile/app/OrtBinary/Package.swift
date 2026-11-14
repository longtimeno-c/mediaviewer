// swift-tools-version: 5.9
// M1 spike S1 (throwaway): ONNX Runtime's static iOS framework, at the version the desktop pins
// (cmake/ai.cmake). Microsoft's Swift package stops at 1.24.2, which cannot load the shipping
// text.onnx (BLOCKERS.md).
import PackageDescription

let package = Package(
  name: "OrtBinary",
  platforms: [.iOS(.v16)],
  products: [.library(name: "OrtBinary", targets: ["onnxruntime"])],
  targets: [
    .binaryTarget(
      name: "onnxruntime",
      url: "https://download.onnxruntime.ai/pod-archive-onnxruntime-c-1.30.0.zip",
      checksum: "e6f1670c14406fd9f082bb400ab197a9b0a9646058ca6366e440642e2b54a2ea")
  ]
)
