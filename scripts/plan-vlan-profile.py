#!/usr/bin/env python3
"""Validate an A1470 VLAN profile and calculate register/table values offline.

No device access, network, shell execution, generated hardware writer, or
implicit zero baselines. Output is design data, not a deployable transaction.
"""
import argparse
import json
from pathlib import Path
import sys

PORTS = frozenset((0, 1, 2, 3, 8))
PORT_MASK = sum(1 << port for port in PORTS)
DEVICE = {"model": "A1470", "firmware": "7.9.1"}
EXTRA = 0x00200000


def read_json(path):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def reject_constant(value):
        raise ValueError(f"nonstandard JSON constant: {value}")

    return json.loads(path.read_text(), object_pairs_hook=unique_object,
                      parse_constant=reject_constant)


def number(value, label, maximum=0xFFFFFFFF, minimum=0):
    if type(value) is int:
        result = value
    elif isinstance(value, str):
        try:
            result = int(value, 16 if value.lower().startswith("0x") else 10)
        except ValueError:
            raise ValueError(f"{label}: expected an integer or hex string") from None
    else:
        raise ValueError(f"{label}: expected an integer or hex string")
    if not minimum <= result <= maximum:
        raise ValueError(f"{label}: must be in {minimum}..{maximum}")
    return result


def vid(value):
    return number(value, "VID", 4094, 1)


def bitmap(values, label):
    if not isinstance(values, list):
        raise ValueError(f"{label}: expected a port list")
    ports = [number(p, label, 8) for p in values]
    if len(ports) != len(set(ports)) or not set(ports) <= PORTS:
        raise ValueError(f"{label}: unique ASIC ports from {sorted(PORTS)} required")
    return sum(1 << port for port in ports)


def selector(page, register, bits):
    return (page << 24) | (register << 16) | {8: 0, 16: 1, 32: 2}[bits]


def register(name, page, offset, bits, value):
    return {"name": name, "page": page, "register": offset, "bits": bits,
            "selector": f"0x{selector(page, offset, bits):08x}", "value": value}


def keys(obj, allowed, required, label):
    if not isinstance(obj, dict):
        raise ValueError(f"{label}: expected an object")
    unknown, missing = set(obj) - set(allowed), set(required) - set(obj)
    if unknown or missing:
        raise ValueError(f"{label}: unknown keys {sorted(unknown)}; missing keys {sorted(missing)}")


def make_plan(profile, baseline=None):
    keys(profile, ("format", "device", "entry_extra_bits", "vlans", "ports", "remove_vids"),
         ("format", "device", "entry_extra_bits", "vlans", "ports"), "profile")
    if profile["format"] != 1 or type(profile["format"]) is not int or profile["device"] != DEVICE:
        raise ValueError("profile: format 1 and A1470 firmware 7.9.1 required")
    if number(profile["entry_extra_bits"], "entry_extra_bits") != EXTRA:
        raise ValueError("entry_extra_bits: this A1470 layout preserves the studied 0x00200000 bits")
    if not isinstance(profile["vlans"], list) or not profile["vlans"]:
        raise ValueError("vlans: a nonempty list is required")
    vlans, warnings = {}, []
    for item in profile["vlans"]:
        keys(item, ("vid", "members", "untagged"), ("vid", "members", "untagged"), "VLAN")
        v = vid(item["vid"])
        members = bitmap(item["members"], "members")
        untag = bitmap(item["untagged"], "untagged")
        if v in vlans or not members or untag & ~members:
            raise ValueError(f"VLAN {v}: duplicate, empty membership, or untagged port outside members")
        vlans[v] = {"vid": v, "members": members, "untagged": untag,
                    "entry": EXTRA | members | (untag << 9)}
    remove_items = profile.get("remove_vids", [])
    if not isinstance(remove_items, list):
        raise ValueError("remove_vids: expected a list")
    remove = [vid(v) for v in remove_items]
    if len(remove) != len(set(remove)) or set(remove) & set(vlans):
        raise ValueError("remove_vids: duplicates or a VID also in target vlans")
    if not isinstance(profile["ports"], list):
        raise ValueError("ports: expected a list")
    ports, target_registers = {}, []
    for item in profile["ports"]:
        keys(item, ("port", "pvid", "pvlan_mask"), ("port", "pvid"), "port")
        p = number(item["port"], "port", 8)
        pvid = vid(item["pvid"])
        if p not in PORTS or p in ports:
            raise ValueError("ports: duplicate or unsupported ASIC port")
        if pvid not in vlans or not vlans[pvid]["members"] & (1 << p):
            raise ValueError(f"port {p}: PVID must name a VLAN containing this port")
        required_mask = 0
        untag_vids = []
        for v in vlans.values():
            if v["members"] & (1 << p):
                required_mask |= v["members"]
            if v["untagged"] & (1 << p):
                untag_vids.append(v["vid"])
        mask = number(item.get("pvlan_mask", required_mask), "pvlan_mask", PORT_MASK)
        if mask & ~PORT_MASK or required_mask & ~mask:
            raise ValueError(f"port {p}: mask must contain all VLAN peers and only supported ports")
        if untag_vids != [pvid]:
            warnings.append(f"port {p}: egress untagged VIDs {sorted(untag_vids)} differ from ingress PVID {pvid}; verify asymmetric tagging deliberately")
        ports[p] = {"port": p, "pvid": pvid, "pvlan_mask": mask,
                    "mask_rule": "explicit_superset" if "pvlan_mask" in item else "union_of_vlan_members"}
        target_registers.extend((register(f"port{p}_mask", 0x31, 2 * p, 16, mask),
                                 register(f"port{p}_pvid", 0x34, 0x10 + 2 * p, 16, pvid)))
    if set(ports) != PORTS:
        raise ValueError(f"ports: specify all managed ASIC ports {sorted(PORTS)}")
    target_registers.extend((register("control5", 0x34, 0x06, 8, 0x18),
                             register("control4", 0x34, 0x05, 8, 0x40)))
    tables = [dict(v, entry_hex=f"0x{v['entry']:08x}") for _, v in sorted(vlans.items())]
    tables += [{"vid": v, "members": 0, "untagged": 0, "entry": 0,
                "entry_hex": "0x00000000", "operation": "explicit_removal"} for v in sorted(remove)]
    plan = {"format": 1, "kind": "offline_vlan_design", "device": dict(DEVICE),
            "deployable": False, "hardware_tested": False, "changes_are_unordered": True,
            "entry_extra_bits": EXTRA, "unmanaged_ports": [5],
            "target": {"vlans": tables, "ports": [ports[p] for p in sorted(ports)],
                       "registers": target_registers},
            "cpu_tagged_vids": [v["vid"] for v in tables if v["members"] & (1 << 8) and not v["untagged"] & (1 << 8)],
            "warnings": warnings,
            "limitations": ["Physical port mappings and endpoint behavior are not verified by this tool.",
                            "Bit21 is retained from the studied device; its complete meaning is unresolved.",
                            "Input baseline coverage is not a full ASIC inventory proof.",
                            "Unmanaged port registers are not targeted; global policies and VLAN changes can still affect those ports.",
                            "PVID and egress tagging do not reject all tagged ingress.",
                            "No hardware writer or boot service for this profile is generated."]}
    if baseline is None:
        plan["baseline_kind"] = "not_supplied"
        plan["changes"] = None
        return plan
    keys(baseline, ("kind", "device", "switch_mode", "control0", "control4", "control5", "ports", "vlans"),
         ("kind", "device", "switch_mode", "control0", "control4", "control5", "ports", "vlans"), "baseline")
    if baseline["kind"] not in ("synthetic", "operator_supplied") or baseline["device"] != DEVICE:
        raise ValueError("baseline: kind synthetic/operator_supplied and matching device required")
    if number(baseline["switch_mode"], "switch_mode", 255) != 7 or number(baseline["control0"], "control0", 255) != 0xE3:
        raise ValueError("baseline: switch_mode/control0 do not match the studied switch mode")
    before_regs = {"control4": number(baseline["control4"], "control4", 255),
                   "control5": number(baseline["control5"], "control5", 255)}
    if before_regs["control4"] not in (0xC4, 0xC0, 0x40) or before_regs["control5"] not in (0x10, 0x18):
        raise ValueError("baseline: unknown control4/control5 mode")
    if not isinstance(baseline["ports"], list) or not isinstance(baseline["vlans"], list):
        raise ValueError("baseline: ports and vlans must be lists")
    seen_ports = set()
    for item in baseline["ports"]:
        keys(item, ("port", "pvid", "pvlan_mask"), ("port", "pvid", "pvlan_mask"), "baseline port")
        p = number(item["port"], "baseline port", 8)
        if p not in PORTS or p in seen_ports:
            raise ValueError("baseline: duplicate or unsupported port")
        seen_ports.add(p)
        before_regs[f"port{p}_pvid"] = vid(item["pvid"])
        before_regs[f"port{p}_mask"] = number(item["pvlan_mask"], "baseline mask", 0x1FF)
    if seen_ports != PORTS:
        raise ValueError("baseline: all managed ports must have observations")
    before_tables = {}
    for item in baseline["vlans"]:
        keys(item, ("vid", "entry"), ("vid", "entry"), "baseline VLAN")
        v = vid(item["vid"])
        if v in before_tables:
            raise ValueError("baseline: duplicate VID")
        before_tables[v] = number(item["entry"], "baseline entry")
    required_vids = set(vlans) | set(remove)
    if required_vids - set(before_tables):
        raise ValueError(f"baseline: missing explicit entry values for {sorted(required_vids - set(before_tables))}; zero is never assumed")
    unexpected = {v for v, value in before_tables.items() if value and v not in required_vids}
    if unexpected:
        raise ValueError(f"baseline: nonzero VIDs {sorted(unexpected)} must be retained in vlans or explicitly removed")
    changes = []
    for v in tables:
        if before_tables[v["vid"]] != v["entry"]:
            changes.append({"kind": "vlan_entry", "vid": v["vid"],
                            "before": before_tables[v["vid"]], "after": v["entry"]})
    for reg in target_registers:
        if before_regs[reg["name"]] != reg["value"]:
            changes.append(dict(reg, kind="register", before=before_regs[reg["name"]], after=reg["value"]))
    plan["baseline_kind"] = baseline["kind"]
    plan["changes"] = changes
    if baseline["kind"] == "synthetic":
        plan["warnings"].append("Synthetic baseline: this diff contains example values, not device observations.")
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", type=Path, help="Optional output JSON; otherwise stdout")
    args = parser.parse_args()
    try:
        profile = read_json(args.profile)
        baseline = read_json(args.baseline) if args.baseline else None
        output = json.dumps(make_plan(profile, baseline), ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.write_text(output)
        else:
            sys.stdout.write(output)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"profile error: {error}\n")


if __name__ == "__main__":
    main()
