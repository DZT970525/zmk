import { useEffect, useState } from "react";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import type { RpcConnection } from "@zmkfirmware/zmk-studio-ts-client";
import type { KeyPhysicalAttrs } from "@zmkfirmware/zmk-studio-ts-client/keymap";

export type BehaviorOption = { id: number; displayName: string };

/**
 * Fetches the active physical layout's key positions via the standard ZMK
 * Studio core RPC (keymap.getPhysicalLayouts), so combo positions can be
 * picked from a rendered layout instead of typed in as raw numbers.
 */
export function usePhysicalLayoutKeys(
  connection: RpcConnection | null | undefined
): KeyPhysicalAttrs[] | null {
  const [keys, setKeys] = useState<KeyPhysicalAttrs[] | null>(null);

  useEffect(() => {
    if (!connection) {
      return undefined;
    }
    let cancelled = false;
    void Promise.resolve(
      call_rpc(connection, { keymap: { getPhysicalLayouts: true } })
    ).then(
      (resp) => {
        if (cancelled) return;
        const layouts = resp?.keymap?.getPhysicalLayouts;
        if (!layouts) return;
        const layout = layouts.layouts[layouts.activeLayoutIndex];
        setKeys(layout?.keys ?? null);
      },
      () => {
        if (!cancelled) setKeys(null);
      }
    );
    return () => {
      cancelled = true;
    };
  }, [connection]);

  return connection ? keys : null;
}

/**
 * Fetches every behavior's id and display name via the standard ZMK Studio
 * core RPCs (behaviors.listAllBehaviors + getBehaviorDetails), so a combo's
 * behavior can be picked by name instead of a raw numeric id.
 */
export function useBehaviorOptions(
  connection: RpcConnection | null | undefined
): BehaviorOption[] | null {
  const [options, setOptions] = useState<BehaviorOption[] | null>(null);

  useEffect(() => {
    if (!connection) {
      return undefined;
    }
    let cancelled = false;
    void (async () => {
      const listResp = await call_rpc(connection, {
        behaviors: { listAllBehaviors: true },
      });
      const ids = listResp?.behaviors?.listAllBehaviors?.behaviors ?? [];
      const details = await Promise.all(
        ids.map((id) =>
          call_rpc(connection, {
            behaviors: { getBehaviorDetails: { behaviorId: id } },
          })
        )
      );
      if (cancelled) return;
      const resolved = details
        .map((resp, index) => ({
          id: ids[index],
          displayName: resp?.behaviors?.getBehaviorDetails?.displayName ?? "",
        }))
        .filter((option) => option.displayName)
        .sort((a, b) => a.displayName.localeCompare(b.displayName));
      setOptions(resolved);
    })().catch(() => {
      if (!cancelled) setOptions(null);
    });
    return () => {
      cancelled = true;
    };
  }, [connection]);

  return connection ? options : null;
}
