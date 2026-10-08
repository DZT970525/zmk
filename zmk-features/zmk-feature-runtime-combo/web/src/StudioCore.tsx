import type { KeyPhysicalAttrs } from "@zmkfirmware/zmk-studio-ts-client/keymap";
import type { BehaviorOption } from "./useStudioCore";

const KEY_UNIT_PX = 40;

/** Renders the physical layout as a grid of clickable keys and toggles
 * `selected` membership on click, replacing raw comma-separated entry. */
export function PositionPicker({
  keys,
  selected,
  onChange,
}: {
  keys: KeyPhysicalAttrs[] | null;
  selected: number[];
  onChange: (positions: number[]) => void;
}) {
  if (!keys || keys.length === 0) {
    return (
      <p className="message">
        Connect to a device to pick positions from its layout.
      </p>
    );
  }

  const toUnits = (value: number) => value / 100;
  const maxX = Math.max(
    ...keys.map((key) => toUnits(key.x) + toUnits(key.width))
  );
  const maxY = Math.max(
    ...keys.map((key) => toUnits(key.y) + toUnits(key.height))
  );

  const toggle = (position: number) => {
    onChange(
      selected.includes(position)
        ? selected.filter((value) => value !== position)
        : [...selected, position]
    );
  };

  return (
    <div
      className="position-picker"
      style={{
        width: maxX * KEY_UNIT_PX,
        height: maxY * KEY_UNIT_PX,
      }}
    >
      {keys.map((key, position) => (
        <button
          key={position}
          type="button"
          className={`position-key ${selected.includes(position) ? "selected" : ""}`}
          style={{
            left: toUnits(key.x) * KEY_UNIT_PX,
            top: toUnits(key.y) * KEY_UNIT_PX,
            width: toUnits(key.width) * KEY_UNIT_PX - 2,
            height: toUnits(key.height) * KEY_UNIT_PX - 2,
          }}
          onClick={() => toggle(position)}
        >
          {position}
        </button>
      ))}
    </div>
  );
}

/** A behavior picker populated by name via the core RPCs, falling back to a
 * plain numeric id input until the behavior list has loaded. */
export function BehaviorSelect({
  options,
  value,
  onChange,
}: {
  options: BehaviorOption[] | null;
  value: number;
  onChange: (behaviorId: number) => void;
}) {
  if (!options) {
    return (
      <input
        type="number"
        min="0"
        value={value}
        onChange={(event) => onChange(Number(event.target.value))}
      />
    );
  }

  return (
    <select
      value={value}
      onChange={(event) => onChange(Number(event.target.value))}
    >
      <option value={0}>Select a behavior...</option>
      {options.map((option) => (
        <option key={option.id} value={option.id}>
          {option.displayName} (#{option.id})
        </option>
      ))}
    </select>
  );
}
