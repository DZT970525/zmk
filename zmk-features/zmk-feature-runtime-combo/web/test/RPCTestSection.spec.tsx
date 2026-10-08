import { render, screen, waitFor } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import {
  Request,
  Response,
} from "../src/proto/cormoran/runtime_combo/runtime_combo";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  call_rpc: jest.fn(),
}));

// Dispatches by request shape (custom subsystem vs. core keymap/behaviors
// RPCs) instead of call order, since RPCTestSection now issues core RPCs
// (position layout, behavior list) concurrently with its own custom RPCs.
function mockCallRpc(customResponses: {
  listCombos?: unknown;
  getGlobalSettings?: unknown;
}) {
  (call_rpc as jest.Mock).mockImplementation(
    async (
      _connection: unknown,
      req: {
        custom?: { call: { payload: Uint8Array } };
        keymap?: unknown;
        behaviors?: unknown;
      }
    ) => {
      if (req.keymap) {
        return {
          keymap: {
            getPhysicalLayouts: { activeLayoutIndex: 0, layouts: [] },
          },
        };
      }
      if (req.behaviors) {
        return { behaviors: { listAllBehaviors: { behaviors: [] } } };
      }
      if (req.custom) {
        const decoded = Request.decode(req.custom.call.payload);
        if (decoded.listCombos && customResponses.listCombos) {
          return {
            custom: {
              call: {
                payload: Response.encode(
                  Response.create({
                    listCombos: customResponses.listCombos,
                  })
                ).finish(),
              },
            },
          };
        }
        if (decoded.getGlobalSettings && customResponses.getGlobalSettings) {
          return {
            custom: {
              call: {
                payload: Response.encode(
                  Response.create({
                    getGlobalSettings: customResponses.getGlobalSettings,
                  })
                ).finish(),
              },
            },
          };
        }
      }
      return undefined;
    }
  );
}

describe("RPCTestSection Component", () => {
  beforeEach(() => {
    jest.clearAllMocks();
    mockCallRpc({
      listCombos: { combos: [] },
      getGlobalSettings: { settings: {} },
    });
  });

  describe("With Subsystem", () => {
    it("should render RPC controls when subsystem is found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: /Runtime Combos/i })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: /Combo Editor/i })
      ).toBeInTheDocument();
      expect(screen.getByText(/^Positions:/i)).toBeInTheDocument();
      expect(screen.getByText(/Save Combo/i)).toBeInTheDocument();
    });

    it("should show default position selection", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(screen.getByText(/^Positions:\s*0 \+ 1/i)).toBeInTheDocument();
    });

    it("should show max combo count from global settings", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });
      mockCallRpc({
        listCombos: { combos: [] },
        getGlobalSettings: {
          settings: {
            timeoutMs: 75,
            slowRelease: true,
            maxCombo: 12,
          },
        },
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByLabelText(/Max combos/i)).toHaveValue(12);
      });
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
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__runtime_combo" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the runtime combo module/i
        )
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<RPCTestSection />);

      expect(container.firstChild).toBeNull();
    });
  });
});
