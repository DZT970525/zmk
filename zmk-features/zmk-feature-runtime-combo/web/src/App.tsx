import { useCallback, useContext, useEffect, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  ComboSource,
  Request,
  Response,
  SlowReleaseOverride,
} from "./proto/cormoran/runtime_combo/runtime_combo";
import type {
  Combo,
  GlobalSettings,
} from "./proto/cormoran/runtime_combo/runtime_combo";
import { usePhysicalLayoutKeys, useBehaviorOptions } from "./useStudioCore";
import { PositionPicker, BehaviorSelect } from "./StudioCore";

export const SUBSYSTEM_IDENTIFIER = "cormoran__runtime_combo";

type ComboDraft = {
  index: number;
  name: string;
  keyPositions: number[];
  behaviorId: number;
  param1: number;
  param2: number;
  layerMask: string;
  enabled: boolean;
  persist: boolean;
  // 0 means inherit the global timeout.
  timeoutMs: number;
  // 0 means inherit the global require-prior-idle setting.
  requirePriorIdleMs: number;
  slowReleaseOverride: SlowReleaseOverride;
  source: ComboSource;
};

type GlobalSettingsDraft = {
  timeoutMs: number;
  slowRelease: boolean;
  maxCombo: number;
  requirePriorIdleMs: number;
  persist: boolean;
};

const emptyDraft: ComboDraft = {
  index: 0,
  name: "",
  keyPositions: [0, 1],
  behaviorId: 0,
  param1: 0,
  param2: 0,
  layerMask: "0",
  enabled: true,
  persist: false,
  timeoutMs: 0,
  requirePriorIdleMs: 0,
  slowReleaseOverride: SlowReleaseOverride.SLOW_RELEASE_OVERRIDE_INHERIT,
  source: ComboSource.COMBO_SOURCE_EMPTY,
};

const COMBO_SOURCE_LABELS: Record<ComboSource, string> = {
  [ComboSource.COMBO_SOURCE_EMPTY]: "Empty",
  [ComboSource.COMBO_SOURCE_DEFAULT]: "Default",
  [ComboSource.COMBO_SOURCE_OVERRIDDEN]: "Overridden",
  [ComboSource.COMBO_SOURCE_RUNTIME]: "Runtime",
  [ComboSource.UNRECOGNIZED]: "Unknown",
};

const emptyGlobalSettings: GlobalSettingsDraft = {
  timeoutMs: 50,
  slowRelease: false,
  maxCombo: 0,
  requirePriorIdleMs: 0,
  persist: false,
};

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>Runtime Combo</h1>
        <p>Custom Studio RPC editor for runtime configurable ZMK combos</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="panel">
            <h2>Device Connection</h2>
            {isLoading && <p>Connecting...</p>}
            {error && <p className="message error">{error}</p>}
            {!isLoading && (
              <button
                className="btn primary"
                onClick={() => connect(serial_connect)}
              >
                Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="panel connection-panel">
              <div>
                <h2>Device Connection</h2>
                <p>Connected to: {deviceName}</p>
              </div>
              <button className="btn" onClick={disconnect}>
                Disconnect
              </button>
            </section>
            <RPCTestSection />
          </>
        )}
      />
    </div>
  );
}

export function RPCTestSection() {
  const zmkApp = useContext(ZMKAppContext);
  const [combos, setCombos] = useState<Combo[]>([]);
  const [globalSettings, setGlobalSettings] =
    useState<GlobalSettingsDraft>(emptyGlobalSettings);
  const [draft, setDraft] = useState<ComboDraft>(emptyDraft);
  const [message, setMessage] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(false);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);
  const connection = zmkApp?.state.connection;
  const subsystemIndex = subsystem?.index;
  const physicalLayoutKeys = usePhysicalLayoutKeys(connection);
  const behaviorOptions = useBehaviorOptions(connection);

  const callRuntimeComboRPC = useCallback(
    async (request: Request): Promise<Response | null> => {
      if (!connection || subsystemIndex === undefined) return null;
      const service = new ZMKCustomSubsystem(connection, subsystemIndex);
      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      return responsePayload ? Response.decode(responsePayload) : null;
    },
    [connection, subsystemIndex]
  );

  const refreshCombos = useCallback(async () => {
    if (!connection || subsystemIndex === undefined) return;
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create({ listCombos: {} })
      );
      if (resp?.listCombos) {
        setCombos(resp.listCombos.combos);
      } else if (resp?.error) {
        setMessage(resp.error.message);
      }
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  }, [callRuntimeComboRPC, connection, subsystemIndex]);

  const refreshGlobalSettings = useCallback(async () => {
    if (!connection || subsystemIndex === undefined) return;
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create({ getGlobalSettings: {} })
      );
      if (resp?.getGlobalSettings?.settings) {
        const settings: GlobalSettings = resp.getGlobalSettings.settings;
        setGlobalSettings({
          timeoutMs: settings.timeoutMs || 50,
          slowRelease: settings.slowRelease,
          maxCombo: settings.maxCombo,
          requirePriorIdleMs: settings.requirePriorIdleMs,
          persist: false,
        });
      } else if (resp?.error) {
        setMessage(resp.error.message);
      }
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  }, [callRuntimeComboRPC, connection, subsystemIndex]);

  useEffect(() => {
    const timer = window.setTimeout(() => {
      void refreshCombos();
      void refreshGlobalSettings();
    }, 0);
    return () => window.clearTimeout(timer);
  }, [refreshCombos, refreshGlobalSettings]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="panel">
        <p className="message warning">
          Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your firmware
          includes the runtime combo module.
        </p>
      </section>
    );
  }

  const selectCombo = (combo: Combo) => {
    setDraft({
      index: combo.index,
      name: combo.name,
      keyPositions: [...combo.keyPositions],
      behaviorId: combo.behavior?.behaviorId ?? 0,
      param1: combo.behavior?.param1 ?? 0,
      param2: combo.behavior?.param2 ?? 0,
      layerMask: `0x${(combo.layerMask ?? 0).toString(16)}`,
      enabled: combo.enabled,
      persist: false,
      timeoutMs: combo.timeoutMs,
      requirePriorIdleMs: combo.requirePriorIdleMs,
      slowReleaseOverride: combo.slowReleaseOverride,
      source: combo.source,
    });
  };

  const parseLayerMask = () => {
    const value = draft.layerMask.trim();
    return value.startsWith("0x")
      ? Number.parseInt(value.slice(2), 16)
      : Number.parseInt(value || "0", 10);
  };

  const saveCombo = async () => {
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create({
          setCombo: {
            index: draft.index,
            keyPositions: draft.keyPositions,
            behavior: {
              behaviorId: draft.behaviorId,
              param1: draft.param1,
              param2: draft.param2,
            },
            layerMask: parseLayerMask(),
            enabled: draft.enabled,
            persist: draft.persist,
            timeoutMs: draft.timeoutMs,
            requirePriorIdleMs: draft.requirePriorIdleMs,
            slowReleaseOverride: draft.slowReleaseOverride,
          },
        })
      );
      if (resp?.error) {
        setMessage(resp.error.message);
        return;
      }
      const nameResp = await callRuntimeComboRPC(
        Request.create({
          setComboName: {
            index: draft.index,
            name: draft.name,
            persist: draft.persist,
          },
        })
      );
      setMessage(nameResp?.error?.message ?? "Combo saved");
      await refreshCombos();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  };

  const saveTimeoutMs = async () => {
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create({
          setTimeoutMs: {
            timeoutMs: globalSettings.timeoutMs,
            persist: globalSettings.persist,
          },
        })
      );
      setMessage(resp?.error?.message ?? "Timeout saved");
      await refreshGlobalSettings();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  };

  const saveSlowRelease = async () => {
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create({
          setSlowRelease: {
            slowRelease: globalSettings.slowRelease,
            persist: globalSettings.persist,
          },
        })
      );
      setMessage(resp?.error?.message ?? "Slow release saved");
      await refreshGlobalSettings();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  };

  const saveRequirePriorIdleMs = async () => {
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create({
          setRequirePriorIdleMs: {
            requirePriorIdleMs: globalSettings.requirePriorIdleMs,
            persist: globalSettings.persist,
          },
        })
      );
      setMessage(resp?.error?.message ?? "Require prior idle saved");
      await refreshGlobalSettings();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  };

  const disableCombo = async () => {
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create({
          deleteCombo: { index: draft.index, persist: draft.persist },
        })
      );
      setMessage(resp?.error?.message ?? "Combo disabled");
      await refreshCombos();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  };

  const resetCombo = async () => {
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create({ resetCombo: { index: draft.index } })
      );
      setMessage(resp?.error?.message ?? "Combo reset to default");
      await refreshCombos();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  };

  const applyPendingSettings = async (action: "save" | "discard") => {
    setIsLoading(true);
    setMessage(null);
    try {
      const resp = await callRuntimeComboRPC(
        Request.create(action === "save" ? { save: {} } : { discard: {} })
      );
      if (resp?.error) {
        setMessage(resp.error.message);
        return;
      }
      const affectedCount = resp?.status?.affectedCount ?? 0;
      setMessage(
        action === "save"
          ? `Saved ${affectedCount} runtime combo settings`
          : `Discarded ${affectedCount} runtime combo settings`
      );
      await refreshCombos();
      await refreshGlobalSettings();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "RPC failed");
    } finally {
      setIsLoading(false);
    }
  };

  const keyboardAbyssPreview = {
    type: "raw",
    zmk: `behavior#${draft.behaviorId} ${draft.param1} ${draft.param2}`,
    label: draft.name || `Combo ${draft.index}`,
  };

  return (
    <main className="runtime-combo">
      <section className="panel">
        <div className="section-heading">
          <div>
            <h2>Runtime Combos</h2>
            <p>{combos.length} configured slots</p>
          </div>
          <button className="btn" disabled={isLoading} onClick={refreshCombos}>
            Refresh
          </button>
        </div>

        <div className="actions">
          <button
            className="btn primary"
            disabled={isLoading}
            onClick={() => applyPendingSettings("save")}
          >
            Save Pending
          </button>
          <button
            className="btn"
            disabled={isLoading}
            onClick={() => applyPendingSettings("discard")}
          >
            Discard Pending
          </button>
        </div>

        <div className="combo-list">
          {combos.length === 0 && <p>No runtime combos configured.</p>}
          {combos.map((combo) => (
            <button
              key={combo.index}
              className={`combo-row ${combo.enabled ? "" : "disabled"}`}
              onClick={() => selectCombo(combo)}
            >
              <span>#{combo.index}</span>
              <strong>{combo.name || "Unnamed combo"}</strong>
              <span>{combo.keyPositions.join(" + ") || "No positions"}</span>
              <span className="badge">{COMBO_SOURCE_LABELS[combo.source]}</span>
            </button>
          ))}
        </div>
      </section>

      <section className="panel editor">
        <div className="section-heading">
          <div>
            <h2>Global Settings</h2>
            <p>Applied to every runtime combo</p>
          </div>
          <button
            className="btn"
            disabled={isLoading}
            onClick={refreshGlobalSettings}
          >
            Refresh
          </button>
        </div>

        <div className="form-grid">
          <label>
            Max combos
            <input type="number" value={globalSettings.maxCombo} readOnly />
          </label>
          <label>
            Timeout ms
            <input
              type="number"
              min="1"
              max="65535"
              value={globalSettings.timeoutMs}
              onChange={(event) =>
                setGlobalSettings({
                  ...globalSettings,
                  timeoutMs: Number(event.target.value),
                })
              }
            />
          </label>
          <label>
            Require prior idle ms (0 disables)
            <input
              type="number"
              min="0"
              max="65535"
              value={globalSettings.requirePriorIdleMs}
              onChange={(event) =>
                setGlobalSettings({
                  ...globalSettings,
                  requirePriorIdleMs: Number(event.target.value),
                })
              }
            />
          </label>
        </div>

        <div className="switches">
          <label>
            <input
              type="checkbox"
              checked={globalSettings.slowRelease}
              onChange={(event) =>
                setGlobalSettings({
                  ...globalSettings,
                  slowRelease: event.target.checked,
                })
              }
            />
            Slow release
          </label>
          <label>
            <input
              type="checkbox"
              checked={globalSettings.persist}
              onChange={(event) =>
                setGlobalSettings({
                  ...globalSettings,
                  persist: event.target.checked,
                })
              }
            />
            Persist global settings
          </label>
        </div>

        <div className="actions">
          <button
            className="btn primary"
            disabled={isLoading}
            onClick={saveTimeoutMs}
          >
            Save Timeout
          </button>
          <button
            className="btn"
            disabled={isLoading}
            onClick={saveSlowRelease}
          >
            Save Slow Release
          </button>
          <button
            className="btn"
            disabled={isLoading}
            onClick={saveRequirePriorIdleMs}
          >
            Save Require Prior Idle
          </button>
        </div>
      </section>

      <section className="panel editor">
        <div className="section-heading">
          <h2>Combo Editor</h2>
          <span className="badge">{COMBO_SOURCE_LABELS[draft.source]}</span>
        </div>
        <div className="form-grid">
          <label>
            Slot
            <input
              type="number"
              min="0"
              value={draft.index}
              onChange={(event) =>
                setDraft({ ...draft, index: Number(event.target.value) })
              }
            />
          </label>
          <label>
            Name
            <input
              value={draft.name}
              maxLength={64}
              onChange={(event) =>
                setDraft({ ...draft, name: event.target.value })
              }
            />
          </label>
          <label>
            Behavior
            <BehaviorSelect
              options={behaviorOptions}
              value={draft.behaviorId}
              onChange={(behaviorId) => setDraft({ ...draft, behaviorId })}
            />
          </label>
          <label>
            Param 1
            <input
              type="number"
              min="0"
              value={draft.param1}
              onChange={(event) =>
                setDraft({ ...draft, param1: Number(event.target.value) })
              }
            />
          </label>
          <label>
            Param 2
            <input
              type="number"
              min="0"
              value={draft.param2}
              onChange={(event) =>
                setDraft({ ...draft, param2: Number(event.target.value) })
              }
            />
          </label>
          <label>
            Layer mask
            <input
              value={draft.layerMask}
              onChange={(event) =>
                setDraft({ ...draft, layerMask: event.target.value })
              }
            />
          </label>
          <label>
            Timeout ms (0 = inherit global)
            <input
              type="number"
              min="0"
              max="65535"
              value={draft.timeoutMs}
              onChange={(event) =>
                setDraft({ ...draft, timeoutMs: Number(event.target.value) })
              }
            />
          </label>
          <label>
            Require prior idle ms (0 = inherit global)
            <input
              type="number"
              min="0"
              max="65535"
              value={draft.requirePriorIdleMs}
              onChange={(event) =>
                setDraft({
                  ...draft,
                  requirePriorIdleMs: Number(event.target.value),
                })
              }
            />
          </label>
          <label>
            Slow release
            <select
              value={draft.slowReleaseOverride}
              onChange={(event) =>
                setDraft({
                  ...draft,
                  slowReleaseOverride: Number(
                    event.target.value
                  ) as SlowReleaseOverride,
                })
              }
            >
              <option value={SlowReleaseOverride.SLOW_RELEASE_OVERRIDE_INHERIT}>
                Inherit global
              </option>
              <option value={SlowReleaseOverride.SLOW_RELEASE_OVERRIDE_ON}>
                On
              </option>
              <option value={SlowReleaseOverride.SLOW_RELEASE_OVERRIDE_OFF}>
                Off
              </option>
            </select>
          </label>
        </div>

        <div className="position-picker-section">
          <p>
            Positions:{" "}
            {draft.keyPositions.length > 0
              ? draft.keyPositions.join(" + ")
              : "none selected"}
          </p>
          <PositionPicker
            keys={physicalLayoutKeys}
            selected={draft.keyPositions}
            onChange={(keyPositions) => setDraft({ ...draft, keyPositions })}
          />
        </div>

        <div className="switches">
          <label>
            <input
              type="checkbox"
              checked={draft.enabled}
              onChange={(event) =>
                setDraft({ ...draft, enabled: event.target.checked })
              }
            />
            Enabled
          </label>
          <label>
            <input
              type="checkbox"
              checked={draft.persist}
              onChange={(event) =>
                setDraft({ ...draft, persist: event.target.checked })
              }
            />
            Persist to settings
          </label>
        </div>

        <div className="actions">
          <button
            className="btn primary"
            disabled={isLoading}
            onClick={saveCombo}
          >
            Save Combo
          </button>
          <button className="btn" disabled={isLoading} onClick={disableCombo}>
            Disable
          </button>
          {(draft.source === ComboSource.COMBO_SOURCE_DEFAULT ||
            draft.source === ComboSource.COMBO_SOURCE_OVERRIDDEN) && (
            <button className="btn" disabled={isLoading} onClick={resetCombo}>
              Reset to Default
            </button>
          )}
        </div>

        {message && <p className="message">{message}</p>}

        <div className="preview">
          <h3>keyboard-abyss binding preview</h3>
          <pre>{JSON.stringify(keyboardAbyssPreview, null, 2)}</pre>
        </div>
      </section>
    </main>
  );
}

export default App;
