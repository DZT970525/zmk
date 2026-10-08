import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";
import { OsDetectionSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import {
  Os,
  Request,
  Response,
  StateResponse,
} from "../src/proto/cormoran/os-detection/os_detection";

function encodedState(overrides: Partial<StateResponse> = {}): Uint8Array {
  const state = StateResponse.create({
    usb: { connected: true, detected: Os.OS_LINUX },
    bleProfiles: [
      {
        index: 0,
        bonded: true,
        connected: false,
        detected: Os.OS_MACOS,
        override: Os.OS_UNKNOWN,
        effective: Os.OS_MACOS,
      },
    ],
    activeProfileIndex: 0,
    currentEffective: Os.OS_LINUX,
    ...overrides,
  });
  return Response.encode(Response.create({ state })).finish();
}

describe("OsDetectionSection Component", () => {
  afterEach(() => {
    jest.restoreAllMocks();
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <OsDetectionSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__os_detection" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the zmk-feature-os-detection module/i
        )
      ).toBeInTheDocument();
    });
  });

  describe("With Subsystem", () => {
    it("should fetch and render state on mount", async () => {
      jest
        .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
        .mockResolvedValue(encodedState());

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <OsDetectionSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByTestId("usb-detected")).toHaveTextContent("Linux");
      });
      expect(screen.getByTestId("ble-detected-0")).toHaveTextContent("macOS");
    });

    it("does not spam RPC calls when findSubsystem returns a fresh object every call (regression: infinite refresh)", async () => {
      const callRPC = jest
        .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
        .mockResolvedValue(encodedState());

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });
      // The real useZMKApp().findSubsystem allocates a new { index, identifier }
      // object on every call; this test helper's default returns the same
      // array element every time, which is why this bug wasn't caught by the
      // other tests. Match production behavior here.
      mockZMKApp.findSubsystem = jest.fn(() => ({
        index: 0,
        identifier: SUBSYSTEM_IDENTIFIER,
      }));

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <OsDetectionSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByTestId("usb-detected")).toHaveTextContent("Linux");
      });

      // Give React plenty of room to keep re-fetching if the effect were
      // retriggering on every render. This is well under POLL_INTERVAL_MS,
      // so any extra calls can only come from the bug, not the real poll.
      await new Promise((resolve) => setTimeout(resolve, 200));

      expect(callRPC.mock.calls.length).toBeLessThanOrEqual(2);
    });

    it("should send set_ble_override with the correct payload", async () => {
      const callRPC = jest
        .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
        .mockResolvedValueOnce(encodedState());

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <OsDetectionSection />
        </ZMKAppProvider>
      );

      const select = await screen.findByLabelText("override-profile-0");

      callRPC.mockResolvedValueOnce(
        Response.encode(
          Response.create({
            setBleOverride: {
              profile: {
                index: 0,
                bonded: true,
                connected: false,
                detected: Os.OS_MACOS,
                override: Os.OS_WINDOWS,
                effective: Os.OS_WINDOWS,
              },
            },
          })
        ).finish()
      );

      const user = userEvent.setup();
      await user.selectOptions(select, String(Os.OS_WINDOWS));

      await waitFor(() => {
        expect(callRPC).toHaveBeenCalledTimes(2);
      });

      const sentPayload = callRPC.mock.calls[1][0] as Uint8Array;
      const sentRequest = Request.decode(sentPayload);
      expect(sentRequest.setBleOverride).toEqual({
        profileIndex: 0,
        os: Os.OS_WINDOWS,
      });
    });
  });
});
