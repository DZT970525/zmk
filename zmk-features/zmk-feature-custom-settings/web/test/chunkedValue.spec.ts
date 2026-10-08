import { writeValueChunked } from "../src/chunkedValue";
import {
  Request,
  Response,
  SettingRef,
  SettingWriteMode,
} from "../src/proto/cormoran/zmk/custom_settings/custom_settings";

const ref: SettingRef = { key: "large_bytes" };

function makeValue(size: number): Uint8Array {
  const out = new Uint8Array(size);
  for (let i = 0; i < size; i++) {
    out[i] = (i * 7 + 3) & 0xff;
  }
  return out;
}

describe("chunked large-value transfer", () => {
  it("splits a value into ordered WriteValueChunk frames and commits the last", async () => {
    const value = makeValue(200);
    const assembled: number[] = [];
    let declaredTotal = -1;
    let committed = false;

    const callRPC = jest.fn(async (request: Request): Promise<Response> => {
      const chunk = request.writeValueChunk!;
      if (chunk.offset === 0) {
        declaredTotal = chunk.totalSize;
      }
      expect(chunk.offset).toBe(assembled.length);
      assembled.push(...Array.from(chunk.data));
      if (chunk.commit) {
        committed = true;
      }
      return Response.create({
        status: { affectedCount: assembled.length, message: "ok" },
      });
    });

    await writeValueChunked(
      callRPC,
      ref,
      value,
      SettingWriteMode.SETTING_WRITE_MODE_PERSIST
    );

    expect(declaredTotal).toBe(value.length);
    expect(committed).toBe(true);
    expect(assembled).toEqual(Array.from(value));
    expect(callRPC).toHaveBeenCalledTimes(2);
  });

  it("throws when a write chunk RPC returns an error", async () => {
    const callRPC = jest.fn(async (): Promise<Response> => {
      return Response.create({ error: { message: "Unlock required" } });
    });

    await expect(
      writeValueChunked(
        callRPC,
        ref,
        makeValue(200),
        SettingWriteMode.SETTING_WRITE_MODE_PERSIST
      )
    ).rejects.toThrow("Unlock required");
  });
});
