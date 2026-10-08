import { useCallback, useContext, useEffect, useMemo, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Os,
  Request,
  Response,
  BleProfileState,
  StateResponse,
} from "./proto/cormoran/os-detection/os_detection";

export const SUBSYSTEM_IDENTIFIER = "cormoran__os_detection";

const POLL_INTERVAL_MS = 3000;

const OS_LABELS: Record<Os, string> = {
  [Os.OS_UNSPECIFIED]: "Unknown",
  [Os.OS_UNKNOWN]: "Unknown",
  [Os.OS_WINDOWS]: "Windows",
  [Os.OS_MACOS]: "macOS",
  [Os.OS_LINUX]: "Linux",
  [Os.OS_IOS]: "iOS",
  [Os.OS_ANDROID]: "Android",
  [Os.UNRECOGNIZED]: "Unknown",
};

const OVERRIDE_OPTIONS = [
  { value: Os.OS_UNKNOWN, label: "AUTO" },
  { value: Os.OS_WINDOWS, label: "Windows" },
  { value: Os.OS_MACOS, label: "macOS" },
  { value: Os.OS_LINUX, label: "Linux" },
  { value: Os.OS_IOS, label: "iOS" },
  { value: Os.OS_ANDROID, label: "Android" },
];

function osLabel(os: Os): string {
  return OS_LABELS[os] ?? "Unknown";
}

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>🖥️ ZMK OS Detection</h1>
        <p>Host OS detection status &amp; BLE overrides</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>⏳ Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>🚨 {error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                🔌 Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>✅ Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <OsDetectionSection />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>zmk-feature-os-detection</strong> - unofficial custom Studio
          RPC module
        </p>
      </footer>
    </div>
  );
}

export function OsDetectionSection() {
  const zmkApp = useContext(ZMKAppContext);
  const [state, setState] = useState<StateResponse | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // findSubsystem() allocates a new object on every call, so calling it
  // directly in the render body would give fetchState a new identity every
  // render (even when nothing changed), retriggering the effect below
  // forever. useMemo keeps `subsystem` referentially stable across renders
  // that don't change zmkApp itself.
  const subsystem = useMemo(
    () => zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER),
    [zmkApp]
  );

  const fetchState = useCallback(async () => {
    if (!zmkApp?.state.connection || !subsystem) return;

    setIsLoading(true);
    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );
      const payload = Request.encode(Request.create({ getState: {} })).finish();
      const responsePayload = await service.callRPC(payload);
      if (!responsePayload) return;

      const resp = Response.decode(responsePayload);
      if (resp.state) {
        setState(resp.state);
        setError(null);
      } else if (resp.error) {
        setError(resp.error.message);
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setIsLoading(false);
    }
  }, [zmkApp, subsystem]);

  useEffect(() => {
    // Fetch-on-mount + poll while connected is the intended behavior here
    // (manual Refresh button covers the on-demand case). fetchState's
    // setState calls are async (after the RPC round-trip), not synchronous
    // effect-body writes, but the rule can't distinguish that through the
    // useCallback indirection.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    fetchState();
    const interval = setInterval(fetchState, POLL_INTERVAL_MS);
    return () => clearInterval(interval);
  }, [fetchState]);

  const sendOverride = async (profileIndex: number, os: Os) => {
    if (!zmkApp?.state.connection || !subsystem) return;

    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );
      const payload = Request.encode(
        Request.create({ setBleOverride: { profileIndex, os } })
      ).finish();
      const responsePayload = await service.callRPC(payload);
      if (!responsePayload) return;

      const resp = Response.decode(responsePayload);
      if (resp.setBleOverride?.profile) {
        const updatedProfile = resp.setBleOverride.profile;
        setState((prev) =>
          prev
            ? {
                ...prev,
                bleProfiles: prev.bleProfiles.map((p) =>
                  p.index === updatedProfile.index ? updatedProfile : p
                ),
              }
            : prev
        );
        setError(null);
      } else if (resp.error) {
        setError(resp.error.message);
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    }
  };

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the zmk-feature-os-detection module.
          </p>
        </div>
      </section>
    );
  }

  return (
    <section className="card">
      <h2>OS Detection State</h2>

      {error && (
        <div className="error-message">
          <p>🚨 {error}</p>
        </div>
      )}

      <button
        className="btn btn-primary"
        disabled={isLoading}
        onClick={fetchState}
      >
        {isLoading ? "⏳ Refreshing..." : "🔄 Refresh"}
      </button>

      {state && (
        <>
          <h3>USB</h3>
          <p>
            Connected: {state.usb?.connected ? "✅" : "❌"} &middot; Detected
            OS:{" "}
            <strong data-testid="usb-detected">
              {osLabel(state.usb?.detected ?? Os.OS_UNKNOWN)}
            </strong>
          </p>

          <h3>BLE Profiles</h3>
          {state.bleProfiles.length === 0 ? (
            <p>No BLE profiles (BLE detection not enabled on this device).</p>
          ) : (
            <table className="ble-profile-table">
              <thead>
                <tr>
                  <th>Index</th>
                  <th>Bonded</th>
                  <th>Connected</th>
                  <th>Detected</th>
                  <th>Override</th>
                  <th>Effective</th>
                </tr>
              </thead>
              <tbody>
                {state.bleProfiles.map((profile: BleProfileState) => (
                  <tr
                    key={profile.index}
                    className={
                      profile.index === state.activeProfileIndex
                        ? "active-profile"
                        : undefined
                    }
                  >
                    <td>{profile.index}</td>
                    <td>{profile.bonded ? "✅" : "❌"}</td>
                    <td>{profile.connected ? "✅" : "❌"}</td>
                    <td data-testid={`ble-detected-${profile.index}`}>
                      {osLabel(profile.detected)}
                    </td>
                    <td>
                      <select
                        aria-label={`override-profile-${profile.index}`}
                        value={profile.override}
                        onChange={(e) =>
                          sendOverride(
                            profile.index,
                            Number(e.target.value) as Os
                          )
                        }
                      >
                        {OVERRIDE_OPTIONS.map((opt) => (
                          <option key={opt.value} value={opt.value}>
                            {opt.label}
                          </option>
                        ))}
                      </select>
                    </td>
                    <td data-testid={`ble-effective-${profile.index}`}>
                      {osLabel(profile.effective)}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}

          <p>
            Current effective OS:{" "}
            <strong>{osLabel(state.currentEffective)}</strong>
          </p>
        </>
      )}
    </section>
  );
}

export default App;
