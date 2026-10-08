import { render, screen, act } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { DeviceInfoPanel, SUBSYSTEM_IDENTIFIER } from "../src/App";

describe("DeviceInfoPanel Component", () => {
  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<DeviceInfoPanel />);

      expect(container.firstChild).toBeNull();
    });
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <DeviceInfoPanel />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/not found/i, { selector: "p" })
      ).toHaveTextContent(SUBSYSTEM_IDENTIFIER);
    });
  });

  describe("With Subsystem", () => {
    it("should show device info panel heading when subsystem is found", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      await act(async () => {
        render(
          <ZMKAppProvider value={mockZMKApp}>
            <DeviceInfoPanel />
          </ZMKAppProvider>
        );
      });

      expect(screen.getByText(/Device Information/i)).toBeInTheDocument();
    });
  });
});
