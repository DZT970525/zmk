import { render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import {
  exportedSettingValueToProto,
  parseSettingsExportJson,
  settingsExportToJson,
  settingToExportedSetting,
} from "../src/settingsJson";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import {
  Notification,
  Request,
  Response,
  Setting,
  SettingConfidentiality,
  SettingNotificationKind,
  SettingPermission,
} from "../src/proto/cormoran/zmk/custom_settings/custom_settings";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  call_rpc: jest.fn(),
}));

const baseSetting: Setting = {
  customSubsystemIndex: 1,
  key: "int_value",
  meta: undefined,
  hasUnsavedValue: false,
  value: { int32Value: 42 },
  source: 0,
};

describe("RPCTestSection Component", () => {
  beforeEach(() => {
    jest.clearAllMocks();
    (call_rpc as jest.Mock).mockResolvedValue(emptyStatusResponse());
  });

  describe("With Subsystem", () => {
    it("should render RPC controls when subsystem is found", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: "Settings" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: "Filter" })
      ).toBeInTheDocument();
      expect(screen.getByLabelText(/Filter Subsystem/i)).toBeInTheDocument();
      expect(screen.getByLabelText(/Filter Key/i)).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: "Device Settings" })
      ).toBeInTheDocument();
      expect(
        screen.getByText(/Settings load automatically/i)
      ).toBeInTheDocument();
      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(1));
      expect(screen.getByLabelText(/Settings JSON/i)).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Export JSON" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Import JSON" })
      ).toBeInTheDocument();
      expect(
        screen.queryByRole("heading", { name: "Update Value" })
      ).not.toBeInTheDocument();
    });

    it("should show value editor under clicked scalar setting", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, baseSetting);

      const settingButton = await screen.findByRole(
        "button",
        { name: "int_value" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);

      const updateValueHeading = screen.getByRole("heading", {
        name: "Update Value",
      });
      expect(updateValueHeading).toBeInTheDocument();
      const updateValuePanel = updateValueHeading.closest(
        "section"
      ) as HTMLElement;
      expect(within(updateValuePanel).getByText("int32")).toBeInTheDocument();
      expect(screen.queryByText("Array Index")).not.toBeInTheDocument();
      expect(screen.queryByText("Array Size")).not.toBeInTheDocument();
      const input = within(updateValuePanel).getByLabelText(
        /Value/i
      ) as HTMLInputElement;
      expect(input.value).toBe("42");
      expect(
        within(updateValuePanel).queryByRole("button", { name: "Push Back" })
      ).not.toBeInTheDocument();
      expect(
        within(updateValuePanel).queryByRole("button", { name: "Pop Back" })
      ).not.toBeInTheDocument();

      await userEvent.click(settingButton);
      expect(
        screen.queryByRole("heading", { name: "Update Value" })
      ).not.toBeInTheDocument();
    });

    it("should group listed settings by key across sources", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        source: 0,
        value: { int32Value: 42 },
      });
      emitListItem(mockZMKApp, {
        ...baseSetting,
        source: 2,
        hasUnsavedValue: true,
        value: { int32Value: 77 },
      });

      const settingButtons = await screen.findAllByRole(
        "button",
        { name: "int_value" },
        { timeout: 2000 }
      );
      expect(settingButtons).toHaveLength(1);
      expect(
        (settingButtons[0].closest("td") as HTMLTableCellElement).rowSpan
      ).toBe(2);

      const table = screen.getByRole("table");
      expect(
        within(table).getByRole("columnheader", { name: "Setting" })
      ).toBeInTheDocument();
      expect(
        within(table).getByRole("columnheader", { name: "Source" })
      ).toBeInTheDocument();
      expect(
        within(table).getByRole("columnheader", { name: "Value" })
      ).toBeInTheDocument();
      expect(
        within(table).getByRole("columnheader", { name: "Unsaved" })
      ).toBeInTheDocument();
      expect(within(table).getByText("local")).toBeInTheDocument();
      expect(within(table).getByText("2")).toBeInTheDocument();
      expect(within(table).getByText("42")).toBeInTheDocument();
      expect(within(table).getByText("77")).toBeInTheDocument();
      expect(within(table).getByText("yes")).toBeInTheDocument();
    });

    it("should render metadata columns and split tables per subsystem", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        customSubsystemIndex: 1,
        key: "brightness",
        meta: {
          confidentiality:
            SettingConfidentiality.SETTING_CONFIDENTIALITY_RPC_PUBLIC,
          readPermission: SettingPermission.SETTING_PERMISSION_UNSECURE,
          writePermission: SettingPermission.SETTING_PERMISSION_SECURE,
          constraints: [
            { range: { min: { int32Value: 0 }, max: { int32Value: 100 } } },
          ],
        },
      });

      // Setting key is shown without the subsystem prefix; the subsystem name
      // is the table heading instead.
      await screen.findByRole("button", { name: "brightness" });
      expect(
        screen.getByRole("heading", { level: 4, name: "test" })
      ).toBeInTheDocument();

      const table = screen.getByRole("table");
      for (const name of ["Confidentiality", "Read", "Write", "Constraints"]) {
        expect(
          within(table).getByRole("columnheader", { name })
        ).toBeInTheDocument();
      }
      expect(within(table).getByText("public")).toBeInTheDocument();
      expect(within(table).getByText("unsecure")).toBeInTheDocument();
      expect(within(table).getByText("secure")).toBeInTheDocument();
      expect(within(table).getByText("range 0–100")).toBeInTheDocument();
    });

    it("should show array commands for clicked array setting", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        key: "array_value",
        value: {
          arrayValue: {
            index: 0,
            size: 1,
            value: { int32Value: 7 },
          },
        },
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "array_value[0]" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);

      expect(screen.getByText("Array Index")).toBeInTheDocument();
      expect(screen.getByText("Array Size")).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Push Back" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Pop Back" })
      ).toBeInTheDocument();
    });

    it("should write the selected split source", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        source: 2,
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "int_value" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);
      expect(screen.getByLabelText(/^Source$/i)).toHaveValue("2");
      await userEvent.click(screen.getByRole("button", { name: "Write" }));

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();

      expect(request.writeSetting?.setting).toMatchObject({
        customSubsystemIndex: 1,
        key: "int_value",
        source: 2,
      });
    });

    it("should write all split sources from the editor source selector", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        source: 2,
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "int_value" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);
      await userEvent.selectOptions(
        screen.getByLabelText(/^Source$/i),
        `${0xffffffff}`
      );
      await userEvent.click(screen.getByRole("button", { name: "Write" }));

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();

      expect(request.writeSetting?.setting).toMatchObject({
        customSubsystemIndex: 1,
        key: "int_value",
        source: 0xffffffff,
      });
    });

    it("should apply scope actions to all listed split sources", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(1));
      await screen.findByText("Listed 0 settings");
      await userEvent.click(screen.getByRole("button", { name: "Save" }));

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();

      expect(request.saveSettings?.scope?.source).toBe(0xffffffff);
    });

    it("should create a keyspace entry from the Create Setting panel", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(1));
      await screen.findByText("Listed 0 settings");

      await userEvent.type(screen.getByLabelText(/^Subsystem$/i), "test");
      await userEvent.type(screen.getByLabelText(/^Key$/i), "macro/my-macro-1");
      const createValueInput = screen.getByLabelText(
        "Value"
      ) as HTMLInputElement;
      await userEvent.clear(createValueInput);
      await userEvent.type(createValueInput, "5");
      await userEvent.click(screen.getByRole("button", { name: "Create" }));

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();

      expect(request.createSetting?.setting).toMatchObject({
        customSubsystemIndex: 1,
        key: "macro/my-macro-1",
      });
      expect(request.createSetting?.value).toEqual({ int32Value: 5 });
    });

    it("should delete the selected setting", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        key: "macro/my-macro-1",
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "macro/my-macro-1" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);
      await userEvent.click(screen.getByRole("button", { name: "Delete" }));

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();

      expect(request.deleteSetting?.setting).toMatchObject({
        customSubsystemIndex: 1,
        key: "macro/my-macro-1",
      });
    });

    it("renders an options dropdown and submits the chosen value", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        key: "mode",
        value: { int32Value: 1 },
        meta: {
          confidentiality: 0,
          readPermission: 0,
          writePermission: 0,
          constraints: [
            {
              options: {
                values: [{ int32Value: 1 }, { int32Value: 2 }],
                labels: ["Low", "High"],
              },
            },
          ],
        },
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "mode" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);

      const panel = getUpdatePanel();
      const select = within(panel).getByLabelText("Value") as HTMLSelectElement;
      expect(select.tagName).toBe("SELECT");
      expect(
        within(select).getByRole("option", { name: "Low" })
      ).toBeInTheDocument();
      expect(
        within(select).getByRole("option", { name: "High" })
      ).toBeInTheDocument();

      await userEvent.selectOptions(select, "2");
      await userEvent.click(screen.getByRole("button", { name: "Write" }));

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();
      expect(request.writeSetting?.value).toEqual({ int32Value: 2 });
    });

    it("warns when an options list may be truncated", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        key: "mode",
        value: { int32Value: 0 },
        meta: {
          confidentiality: 0,
          readPermission: 0,
          writePermission: 0,
          constraints: [
            {
              options: {
                values: Array.from({ length: 8 }, (_, i) => ({
                  int32Value: i,
                })),
                labels: Array.from({ length: 8 }, (_, i) => `opt-${i}`),
              },
            },
          ],
        },
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "mode" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);

      expect(
        screen.getByText(/Option list may be truncated/i)
      ).toBeInTheDocument();
    });

    it("renders a range control and blocks out-of-range submits", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        key: "level",
        value: { int32Value: 5 },
        meta: {
          confidentiality: 0,
          readPermission: 0,
          writePermission: 0,
          constraints: [
            {
              range: { min: { int32Value: 0 }, max: { int32Value: 10 } },
            },
          ],
        },
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "level" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);

      const panel = getUpdatePanel();
      const input = within(panel).getByLabelText("Value") as HTMLInputElement;
      expect(input.type).toBe("number");
      expect(input.min).toBe("0");
      expect(input.max).toBe("10");

      await userEvent.clear(input);
      await userEvent.type(input, "20");
      expect(within(panel).getByRole("alert")).toHaveTextContent(/≤ 10/);
      expect(screen.getByRole("button", { name: "Write" })).toBeDisabled();

      await userEvent.clear(input);
      await userEvent.type(input, "8");
      expect(within(panel).queryByRole("alert")).not.toBeInTheDocument();

      await userEvent.click(screen.getByRole("button", { name: "Write" }));
      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();
      expect(request.writeSetting?.value).toEqual({ int32Value: 8 });
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
        screen.getByText(/Subsystem "cormoran_custom_settings" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the custom settings module/i
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

function emptyStatusResponse() {
  return {
    custom: {
      call: {
        payload: Response.encode(
          Response.create({ status: { affectedCount: 0, message: "OK" } })
        ).finish(),
      },
    },
  };
}

function emitListItem(
  mockZMKApp: ReturnType<typeof createConnectedMockZMKApp>,
  setting: Setting
) {
  const subscription = (mockZMKApp.onNotification as jest.Mock).mock
    .calls[0][0];
  subscription.callback({
    subsystemIndex: 0,
    payload: Notification.encode(
      Notification.create({
        setting: {
          kind: SettingNotificationKind.SETTING_NOTIFICATION_KIND_LIST_ITEM,
          setting,
        },
      })
    ).finish(),
  });
}

function getUpdatePanel(): HTMLElement {
  return screen
    .getByRole("heading", { name: "Update Value" })
    .closest("section") as HTMLElement;
}

function lastCustomSettingsRequest(): Request {
  const calls = (call_rpc as jest.Mock).mock.calls;
  const request = calls[calls.length - 1][1];
  return Request.decode(request.custom.call.payload);
}

describe("settings JSON conversion", () => {
  it("exports scalar settings as typed JSON entries", () => {
    expect(settingToExportedSetting(baseSetting, () => "test")).toEqual({
      customSubsystemId: "test",
      key: "int_value",
      source: 0,
      type: "int32",
      value: 42,
    });
  });

  it("exports array settings with public key and explicit index", () => {
    expect(
      settingToExportedSetting(
        {
          ...baseSetting,
          key: "array_value",
          source: 2,
          value: {
            arrayValue: {
              index: 1,
              size: 2,
              value: { boolValue: true },
            },
          },
        },
        () => "test"
      )
    ).toEqual({
      customSubsystemId: "test",
      key: "array_value",
      source: 2,
      type: "bool",
      value: true,
      arrayIndex: 1,
      arraySize: 2,
    });
  });

  it("exports JSON grouped by customSubsystems as keyed object", () => {
    const json = settingsExportToJson([baseSetting], () => "test");
    const parsed = JSON.parse(json);

    expect(parsed.version).toBe(1);
    expect(parsed.customSubsystems).toBeDefined();
    expect(parsed.customSubsystems["test"]).toEqual({
      int_value: { type: "int32", source: 0, value: 42 },
    });
  });

  it("exports array settings grouped by key with size and value array", () => {
    const arraySetting = {
      ...baseSetting,
      key: "array_value",
      source: 2,
      value: {
        arrayValue: { index: 0, size: 2, value: { boolValue: false } },
      },
    };
    const arraySetting1 = {
      ...baseSetting,
      key: "array_value",
      source: 2,
      value: {
        arrayValue: { index: 1, size: 2, value: { boolValue: true } },
      },
    };
    const json = settingsExportToJson(
      [arraySetting, arraySetting1],
      () => "test"
    );
    const parsed = JSON.parse(json);

    expect(parsed.customSubsystems["test"]).toEqual({
      array_value: { type: "bool", source: 2, size: 2, value: [false, true] },
    });
  });

  it("parses exported JSON back to write values", () => {
    const json = settingsExportToJson([baseSetting], () => "test");
    const [setting] = parseSettingsExportJson(json);

    expect(setting).toMatchObject({
      customSubsystemId: "test",
      key: "int_value",
      source: 0,
      type: "int32",
      value: 42,
    });
    expect(exportedSettingValueToProto(setting)).toEqual({ int32Value: 42 });
  });

  it("parses array settings from new format back to write values", () => {
    const doc = JSON.stringify({
      format: "zmk-custom-settings",
      version: 1,
      exportedAt: new Date().toISOString(),
      customSubsystems: {
        test: {
          array_value: {
            type: "bool",
            source: 2,
            size: 2,
            value: [false, true],
          },
        },
      },
    });
    const settings = parseSettingsExportJson(doc);

    expect(settings).toHaveLength(2);
    expect(settings[0]).toMatchObject({
      customSubsystemId: "test",
      key: "array_value",
      source: 2,
      type: "bool",
      value: false,
      arrayIndex: 0,
      arraySize: 2,
    });
    expect(settings[1]).toMatchObject({
      key: "array_value",
      source: 2,
      value: true,
      arrayIndex: 1,
      arraySize: 2,
    });
  });

  it("rejects JSON with wrong version", () => {
    const old = JSON.stringify({
      format: "zmk-custom-settings",
      version: 2,
      exportedAt: new Date().toISOString(),
      customSubsystems: { test: [{ key: "x", type: "int32", value: 1 }] },
    });
    expect(() => parseSettingsExportJson(old)).toThrow("Unsupported version 2");
  });
});
