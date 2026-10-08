// jest-dom adds custom jest matchers for asserting on DOM nodes.
import "@testing-library/jest-dom";

// jsdom doesn't provide TextEncoder/TextDecoder, needed by protobuf-es's
// BinaryWriter/BinaryReader (used by the generated os_detection.ts client).
import { TextEncoder, TextDecoder } from "node:util";

if (typeof globalThis.TextEncoder === "undefined") {
  // @ts-expect-error -- Node's TextDecoder is a structural match for the DOM one
  globalThis.TextEncoder = TextEncoder;
  // @ts-expect-error -- Node's TextDecoder is a structural match for the DOM one
  globalThis.TextDecoder = TextDecoder;
}
