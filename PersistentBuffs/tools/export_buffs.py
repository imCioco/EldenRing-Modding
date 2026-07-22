#!/usr/bin/env python3
r"""Offline buff-discovery export / validation for PersistentBuffs.

Decodes a regulation.bin and reproduces, in Python, EXACTLY what the mod's
`build_tracked_speffects()` (src/buff_discovery.cpp) does at runtime -- so the
tracked-SpEffect allowlist, the physick-tear group, the weapon-bound vs
character-wide split, and the hard-coded `g_always_persist` seed can all be
inspected, diffed, and regenerated WITHOUT launching the game.

Why this exists
---------------
The C++ discovery is param-driven and overhaul-proof, but you can only see what
it decided by reading the in-game log. This tool runs the same rules against any
regulation (vanilla, a DLC build, or an overhaul like Convergence / Reforged) so
you can:
  * verify a buff you expect IS tracked (and by which source), or find why it
    isn't (debuff? foe-only? not reachable from a source chain?);
  * regenerate the `g_always_persist` character-wide list and DIFF it against
    what's hard-coded in src/config.hpp (surfacing new ashes an overhaul added,
    e.g. Oath of Vengeance, that should be character-wide);
  * dump per-category id lists for a compatibility pass with sibling mods
    (InfiniteWeaponBuffs shares the same param logic).

It reuses the SoulsFormats pipeline vendored in the sibling ERR-MapForGoblins-DLL
repo (Andre.SoulsFormats.dll) and the Paramdex paramdefs already vendored in this
mod (external/Paramdex/ER/Defs), so no extra data needs downloading.

Setup
-----
    pip install pythonnet            # needs the .NET runtime (coreclr)

Point it at a regulation.bin (loose, UXM-unpacked, or a mod's) -- either pass
--regulation, or set it in tools/export_buffs.ini (copied from the .example on
first run). The SoulsFormats DLL + paramdefs default to repo-relative paths;
override with --soulsformats / --paramdefs if your layout differs.

Usage
-----
    python export_buffs.py --check                 # plumbing self-test, no reg
    python export_buffs.py --regulation PATH        # full report to stdout
    python export_buffs.py --regulation PATH --json out.json --diff-always
"""
from __future__ import annotations

import argparse
import configparser
import io
import json
import os
import sys
import tempfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
MOD_DIR = TOOLS_DIR.parent
REPO_DIR = MOD_DIR.parent

# Repo-relative defaults (overridable via CLI / ini).
DEFAULT_SOULSFORMATS = REPO_DIR / "ERR-MapForGoblins-DLL" / "tools" / "lib" / "Andre.SoulsFormats.dll"
DEFAULT_PARAMDEFS = MOD_DIR / "external" / "Paramdex" / "ER" / "Defs"
DEFAULT_NAMES = MOD_DIR / "external" / "Paramdex" / "ER" / "Names" / "SpEffectParam.txt"
CONFIG_HPP = MOD_DIR / "src" / "config.hpp"


# ======================================================================
#  Classification constants -- kept in lock-step with the C++ source.
#  If you change a rule in buff_filters.cpp / buff_discovery.cpp, mirror it here.
# ======================================================================
K_SORT_GROUP_GREASE = 70          # buff_filters.hpp kSortGroupGrease
K_GOODS_TYPE_PHYSICK_TEAR = 10    # buff_discovery.cpp kGoodsTypePhysickTear
K_GOODS_TYPE_SPIRIT_SUMMON = 7    # buff_discovery.cpp kGoodsTypeSpiritSummon (spirit ashes)
K_HORSE_SUMMON_GOODS = {130}      # Spectral Steed Whistle
K_LANTERN_GOODS = 2070
K_LANTERN_SPEFFECTS = (3245, 3246)
K_REF_BULLET = 1                  # refCategory / refType Projectile
K_REF_SPEFFECT = 2               # refType SpEffect
K_CHAIN_MAX_DEPTH = 8            # buff_filters.hpp kChainMaxDepth
K_BULLET_MAX_DEPTH = 3          # buff_discovery.cpp kBulletMaxDepth

# Fields pulled per param row (allowlist keeps the read fast).
SPEFFECT_FIELDS = {
    "effectEndurance", "spCategory",
    "replaceSpEffectId", "cycleOccurrenceSpEffectId",
    "effectTargetSelf", "effectTargetPlayer", "effectTargetFriend",
    "effectTargetSelfTarget", "effectTargetFriendlyTarget",
    "effectTargetEnemy", "effectTargetOpposeTarget",
    "vfxId", "vfxId1", "vfxId2", "vfxId3", "vfxId4", "vfxId5", "vfxId6", "vfxId7",
    # is_beneficial_buff (kept for the report, not the gate)
    "physicsAttackPowerRate", "magicAttackPowerRate", "fireAttackPowerRate",
    "thunderAttackPowerRate", "physicsAttackPower", "magicAttackPower",
    "fireAttackPower", "thunderAttackPower", "physicsAttackRate",
    "magicAttackRate", "fireAttackRate", "thunderAttackRate", "staminaAttackRate",
    "physicsDiffenceRate", "magicDiffenceRate", "fireDiffenceRate",
    "thunderDiffenceRate", "physicsDiffence", "magicDiffence", "fireDiffence",
    "thunderDiffence", "slashDamageCutRate", "blowDamageCutRate",
    "thrustDamageCutRate", "neutralDamageCutRate", "magicDamageCutRate",
    "fireDamageCutRate", "thunderDamageCutRate", "maxHpRate", "maxMpRate",
    "maxStaminaRate", "hpRecoverRate", "mpRecoverChangeSpeed",
    "staminaRecoverChangeSpeed", "registPoizonChangeRate",
    "registDiseaseChangeRate", "registBloodChangeRate", "registCurseChangeRate",
    "haveSoulRate", "soulRate",
    # is_debuff extras
    "changeHpRate", "changeHpPoint", "changeMpRate", "changeMpPoint",
    "changeStaminaRate", "changeStaminaPoint",
    "darkAttackPowerRate", "darkAttackPower", "darkDiffenceRate", "darkDiffence",
    "darkDamageCutRate", "registFreezeChangeRate", "registSleepChangeRate",
    "registMadnessChangeRate", "changePoisonResistPoint",
    "changeDiseaseResistPoint", "changeBloodResistPoint", "changeCurseResistPoint",
    "changeFreezeResistPoint", "changeSleepResistPoint", "changeMadnessResistPoint",
    "changeStrengthPoint", "changeAgilityPoint", "changeMagicPoint",
    "changeFaithPoint", "changeLuckPoint",
}
GOODS_FIELDS = {
    "isEnhance", "isShieldEnchant", "sortGroupId", "refId_default", "refId_1",
    "refCategory", "behaviorId", "goodsType", "isSummonHorse",
}
MAGIC_FIELDS = {f"refId{i}" for i in range(1, 11)} | {f"refCategory{i}" for i in range(1, 11)}
BULLET_FIELDS = {"spEffectIDForShooter", "spEffectId0", "spEffectId1",
                 "spEffectId2", "spEffectId3", "spEffectId4", "HitBulletID"}
BEHAVIOR_FIELDS = {"refType", "refId"}
VFX_FIELDS = {"enchantStartDmyId_0", "soulParamIdForWepEnchant"}


# ======================================================================
#  SoulsFormats bridge (pythonnet) -- mirrors ERR-MapForGoblins-DLL tooling.
# ======================================================================
def load_soulsformats(dll_path: Path):
    from pythonnet import load as _pyload
    _pyload("coreclr")
    from System.Reflection import Assembly
    from System import Array, Type as SysType, Object
    from System.Reflection import BindingFlags
    from System.IO import File as SysFile

    asm = Assembly.LoadFrom(str(dll_path))
    import SoulsFormats  # noqa: F401

    str_t = SysType.GetType("System.String")

    def get_read_str(type_name):
        cls = asm.GetType(type_name)
        return cls.GetMethod(
            "Read",
            BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
            None, Array[SysType]([str_t]), None)

    param_read = get_read_str("SoulsFormats.PARAM")

    def read_from_bytes(read_method, data, suffix=".param"):
        tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_pb_tmp{suffix}")
        SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, "ToArray") else data)
        try:
            return read_method.Invoke(None, Array[Object]([tmp]))
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass

    return {
        "asm": asm, "SoulsFormats": SoulsFormats,
        "param_read": param_read, "read_from_bytes": read_from_bytes,
    }


def load_paramdefs(sf, paramdefs_dir: Path):
    SoulsFormats = sf["SoulsFormats"]
    defs = {}
    for xml in Path(paramdefs_dir).glob("*.xml"):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
            if pdef and pdef.ParamType:
                defs[str(pdef.ParamType)] = pdef
        except Exception:
            pass
    return defs


def decrypt_regulation(sf, reg_path: Path):
    return sf["SoulsFormats"].SFUtil.DecryptERRegulation(str(reg_path))


def read_param(sf, bnd, name, paramdefs):
    """Read a named .param out of the regulation BND, applying its paramdef."""
    for f in bnd.Files:
        if name in str(f.Name):
            param = sf["read_from_bytes"](sf["param_read"], f.Bytes, ".param")
            pt = str(param.ParamType) if param.ParamType else ""
            if pt in paramdefs:
                param.ApplyParamdef(paramdefs[pt])
            return param
    return None


def param_to_dict(param, fields):
    """{row_id: {field: value}} for the requested fields only."""
    out = {}
    if param is None:
        return out
    for row in param.Rows:
        entry = {}
        if row.Cells:
            for cell in row.Cells:
                fn = str(cell.Def.InternalName)
                if fn in fields:
                    v = cell.Value
                    s = str(v)
                    if hasattr(v, "ToString"):
                        # bools -> 0/1; numeric strings -> int/float
                        if s in ("True", "False"):
                            v = 1 if s == "True" else 0
                        else:
                            try:
                                v = int(s)
                            except ValueError:
                                try:
                                    v = float(s)
                                except ValueError:
                                    v = s
                    entry[fn] = v
        out[int(row.ID)] = entry
    return out


# ======================================================================
#  Classification -- mirrors src/buff_filters.cpp + buff_discovery.cpp.
# ======================================================================
def _f(row, key, default=0.0):
    v = row.get(key, default)
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def is_grease(g):
    return bool(g.get("isEnhance") or g.get("isShieldEnchant")
                or int(_f(g, "sortGroupId")) == K_SORT_GROUP_GREASE)


def is_self_buff(r):
    return bool(r.get("effectTargetSelf") or r.get("effectTargetPlayer"))


def is_foe_only(r):
    ally = any(r.get(k) for k in (
        "effectTargetSelf", "effectTargetPlayer", "effectTargetFriend",
        "effectTargetSelfTarget", "effectTargetFriendlyTarget"))
    foe = any(r.get(k) for k in ("effectTargetEnemy", "effectTargetOpposeTarget"))
    return bool(foe and not ally)


def is_debuff(r):
    # SIGN CONVENTION (verified vs vanilla regulation): NEGATIVE change* = restore
    # (heal/regen), POSITIVE = damage/drain. So the debuff is the POSITIVE case.
    # (Was "< 0" -- inverted; dropped every regen buff. Kept in lock-step with
    # buff_filters.cpp is_debuff.)
    if _f(r, "changeHpRate") > 0 or _f(r, "changeHpPoint") > 0:
        return True
    if _f(r, "changeMpRate") > 0 or _f(r, "changeMpPoint") > 0:
        return True
    if _f(r, "changeStaminaRate") > 0 or _f(r, "changeStaminaPoint") > 0:
        return True
    if _f(r, "maxHpRate", 1) < 1 or _f(r, "maxMpRate", 1) < 1 or _f(r, "maxStaminaRate", 1) < 1:
        return True
    if _f(r, "hpRecoverRate") < 0 or _f(r, "mpRecoverChangeSpeed") < 0 or _f(r, "staminaRecoverChangeSpeed") < 0:
        return True
    if any(_f(r, k, 1) < 1 for k in ("physicsAttackPowerRate", "magicAttackPowerRate",
            "fireAttackPowerRate", "thunderAttackPowerRate", "darkAttackPowerRate")):
        return True
    if any(_f(r, k) < 0 for k in ("physicsAttackPower", "magicAttackPower",
            "fireAttackPower", "thunderAttackPower", "darkAttackPower")):
        return True
    if any(_f(r, k, 1) < 1 for k in ("physicsDiffenceRate", "magicDiffenceRate",
            "fireDiffenceRate", "thunderDiffenceRate", "darkDiffenceRate")):
        return True
    if any(_f(r, k) < 0 for k in ("physicsDiffence", "magicDiffence",
            "fireDiffence", "thunderDiffence", "darkDiffence")):
        return True
    if any(_f(r, k, 1) > 1 for k in ("slashDamageCutRate", "blowDamageCutRate",
            "thrustDamageCutRate", "neutralDamageCutRate", "magicDamageCutRate",
            "fireDamageCutRate", "thunderDamageCutRate", "darkDamageCutRate")):
        return True
    if any(_f(r, k, 1) < 1 for k in ("registPoizonChangeRate", "registDiseaseChangeRate",
            "registBloodChangeRate", "registCurseChangeRate", "registFreezeChangeRate",
            "registSleepChangeRate", "registMadnessChangeRate")):
        return True
    if any(_f(r, k) < 0 for k in ("changePoisonResistPoint", "changeDiseaseResistPoint",
            "changeBloodResistPoint", "changeCurseResistPoint", "changeFreezeResistPoint",
            "changeSleepResistPoint", "changeMadnessResistPoint")):
        return True
    if any(_f(r, k) < 0 for k in ("changeStrengthPoint", "changeAgilityPoint",
            "changeMagicPoint", "changeFaithPoint", "changeLuckPoint")):
        return True
    return False


def is_beneficial_buff(r):
    if any(_f(r, k, 1) > 1 for k in ("physicsAttackPowerRate", "magicAttackPowerRate",
            "fireAttackPowerRate", "thunderAttackPowerRate")):
        return True
    if any(_f(r, k) > 0 for k in ("physicsAttackPower", "magicAttackPower",
            "fireAttackPower", "thunderAttackPower")):
        return True
    if any(_f(r, k, 1) > 1 for k in ("physicsAttackRate", "magicAttackRate",
            "fireAttackRate", "thunderAttackRate", "staminaAttackRate")):
        return True
    if any(_f(r, k, 1) > 1 for k in ("physicsDiffenceRate", "magicDiffenceRate",
            "fireDiffenceRate", "thunderDiffenceRate")):
        return True
    if any(_f(r, k) > 0 for k in ("physicsDiffence", "magicDiffence",
            "fireDiffence", "thunderDiffence")):
        return True
    if any(_f(r, k, 1) < 1 for k in ("slashDamageCutRate", "blowDamageCutRate",
            "thrustDamageCutRate", "neutralDamageCutRate", "magicDamageCutRate",
            "fireDamageCutRate", "thunderDamageCutRate")):
        return True
    if _f(r, "maxHpRate", 1) > 1 or _f(r, "maxMpRate", 1) > 1 or _f(r, "maxStaminaRate", 1) > 1:
        return True
    if _f(r, "hpRecoverRate") > 0 or _f(r, "mpRecoverChangeSpeed") > 0 or _f(r, "staminaRecoverChangeSpeed") > 0:
        return True
    if any(_f(r, k, 1) > 1 for k in ("registPoizonChangeRate", "registDiseaseChangeRate",
            "registBloodChangeRate", "registCurseChangeRate")):
        return True
    if _f(r, "haveSoulRate", 1) > 1 or _f(r, "soulRate") > 0:
        return True
    return False


def is_system_effect(i):
    if 100000 <= i <= 100999:
        return True
    if 131 <= i <= 147:
        return True
    if 170 <= i <= 176:
        return True
    if 20200000 <= i <= 20299999:
        return True
    if i == 70:
        return True
    if 500 <= i <= 507:
        return True
    if 6400 <= i <= 6810:
        return True
    return i in {45, 514, 190, 8001, 9540, 10665, 530007, 530012, 9621, 4600}


class Discovery:
    """Reproduces build_tracked_speffects() over a set of param dicts."""

    def __init__(self, sp, goods, magic, bullet, behavior_pc, behavior, vfx):
        self.sp = sp
        self.goods = goods
        self.magic = magic
        self.bullet = bullet
        self.behavior_pc = behavior_pc
        self.behavior = behavior
        self.vfx = vfx
        # Source-bit tags, per id (parallel to g_source_of).
        self.src = {}
        self.enchant_vfx = set()      # kSrcEnchantVfx-tagged ids
        self.enchant_categories = set()
        self.tracked = set()
        self.physick = set()
        self.counts = {"grease": 0, "spell": 0, "consum": 0, "skill": 0,
                       "physick": 0, "utility": 0}

    # ---- chain / bullet walking ----
    def vfx_row_is_enchant(self, vfx_id):
        if vfx_id is None or vfx_id < 0:
            return False
        v = self.vfx.get(vfx_id)
        if not v:
            return False
        return _f(v, "enchantStartDmyId_0", -1) >= 0 or _f(v, "soulParamIdForWepEnchant") > 0

    def has_enchant_vfx(self, r):
        if not r:
            return False
        for k in ("vfxId", "vfxId1", "vfxId2", "vfxId3", "vfxId4",
                  "vfxId5", "vfxId6", "vfxId7"):
            if self.vfx_row_is_enchant(int(_f(r, k, -1))):
                return True
        return False

    def add_bullet_speffects(self, bullet_id, out, depth=0):
        if depth > K_BULLET_MAX_DEPTH:
            return
        bl = self.bullet.get(bullet_id)
        if not bl:
            return
        for k in ("spEffectIDForShooter", "spEffectId0", "spEffectId1",
                  "spEffectId2", "spEffectId3", "spEffectId4"):
            s = int(_f(bl, k, -1))
            if s >= 0:
                out.append(s)
        hb = int(_f(bl, "HitBulletID", -1))
        if hb >= 0 and hb != bullet_id:
            self.add_bullet_speffects(hb, out, depth + 1)

    def collect_all_chain(self, start, out, depth=0):
        if start is None or start < 0 or depth > K_CHAIN_MAX_DEPTH:
            return
        if start in out:
            return
        out.add(start)
        r = self.sp.get(start)
        if not r:
            return
        self.collect_all_chain(int(_f(r, "replaceSpEffectId", -1)), out, depth + 1)
        self.collect_all_chain(int(_f(r, "cycleOccurrenceSpEffectId", -1)), out, depth + 1)

    def gather_goods_entry_speffects(self, g, out):
        for k in ("refId_default", "refId_1"):
            ref = int(_f(g, k, -1))
            if ref < 0:
                continue
            if ref in self.sp:
                out.append(ref)
            if int(_f(g, "refCategory")) == K_REF_BULLET:
                self.add_bullet_speffects(ref, out)
        beh = int(_f(g, "behaviorId", -1))
        if beh > 0:
            b = self.behavior_pc.get(beh) or self.behavior.get(beh)
            if b:
                if int(_f(b, "refType")) == K_REF_SPEFFECT and int(_f(b, "refId", -1)) >= 0:
                    out.append(int(_f(b, "refId")))
                elif int(_f(b, "refType")) == K_REF_BULLET:
                    self.add_bullet_speffects(int(_f(b, "refId", -1)), out)

    def track_entries(self, entries, src_bit, trusted):
        # enchant-vfx tagging, per entry chain
        for e in entries:
            one = set()
            self.collect_all_chain(e, one)
            if any(self.has_enchant_vfx(self.sp.get(n)) for n in one):
                for n in one:
                    if n in self.sp:
                        self.enchant_vfx.add(n)
        chain = set()
        for e in entries:
            self.collect_all_chain(e, chain)
        added = 0
        for n in chain:
            r = self.sp.get(n)
            if not r:
                continue
            if not trusted and (is_debuff(r) or is_foe_only(r)):
                continue
            self.src[n] = self.src.get(n, 0) | src_bit
            if n not in self.tracked:
                self.tracked.add(n)
                added += 1
        return added

    # Source bits (match SourceBits enum values, incl. the removed great-rune gap)
    GREASE, SPELL, CONSUM, BEHAVIOR, UTILITY = 1, 2, 4, 8, 32

    def run(self):
        # 1. greases
        for gid, g in self.goods.items():
            if not is_grease(g):
                continue
            entries = [int(_f(g, k, -1)) for k in ("refId_default", "refId_1")]
            entries = [e for e in entries if e >= 0 and e in self.sp]
            self.counts["grease"] += self.track_entries(entries, self.GREASE, True)
        for sid, bits in list(self.src.items()):
            if not (bits & self.GREASE):
                continue
            r = self.sp.get(sid)
            if r and int(_f(r, "spCategory")) > 0 and self.has_enchant_vfx(r):
                self.enchant_categories.add(int(_f(r, "spCategory")))

        # 2. spells
        for mid, m in self.magic.items():
            direct, from_bullets = [], []
            for i in range(1, 11):
                ref = int(_f(m, f"refId{i}", -1))
                if ref < 0:
                    continue
                if int(_f(m, f"refCategory{i}")) == K_REF_BULLET:
                    self.add_bullet_speffects(ref, from_bullets)
                elif ref in self.sp:
                    direct.append(ref)
            self.counts["spell"] += self.track_entries(direct, self.SPELL, True)
            self.counts["spell"] += self.track_entries(from_bullets, self.SPELL, False)

        # 3. consumables
        for gid, g in self.goods.items():
            if is_grease(g):
                continue
            if g.get("isSummonHorse") or gid in K_HORSE_SUMMON_GOODS:
                continue
            if int(_f(g, "goodsType", -1)) == K_GOODS_TYPE_SPIRIT_SUMMON:
                continue  # spirit ashes: summon state, not a buff
            is_tear = int(_f(g, "goodsType", -1)) == K_GOODS_TYPE_PHYSICK_TEAR
            entries = []
            self.gather_goods_entry_speffects(g, entries)
            chain = set()
            for e in entries:
                self.collect_all_chain(e, chain)
            for n in chain:
                r = self.sp.get(n)
                # Gate = accept-all-but-debuff (is_debuff || is_foe_only), NOT the
                # old is_self_buff requirement -- a self-applied consumable can
                # have NO target flag (Starlight Shards 501290). Lock-step with
                # buff_discovery.cpp.
                if not r or is_debuff(r) or is_foe_only(r):
                    continue
                self.src[n] = self.src.get(n, 0) | self.CONSUM
                if is_tear and n not in self.physick:
                    self.physick.add(n)
                    self.counts["physick"] += 1
                if n not in self.tracked:
                    self.tracked.add(n)
                    self.counts["consum"] += 1

        # 4. weapon skills / ashes of war
        for table in (self.behavior_pc, self.behavior):
            for bid, b in table.items():
                entries = []
                if int(_f(b, "refType")) == K_REF_SPEFFECT and int(_f(b, "refId", -1)) >= 0:
                    entries.append(int(_f(b, "refId")))
                elif int(_f(b, "refType")) == K_REF_BULLET:
                    self.add_bullet_speffects(int(_f(b, "refId", -1)), entries)
                if entries:
                    self.counts["skill"] += self.track_entries(entries, self.BEHAVIOR, False)

        # 5. utility -- the Lantern light
        entries = []
        lantern = self.goods.get(K_LANTERN_GOODS)
        if lantern:
            self.gather_goods_entry_speffects(lantern, entries)
        entries += [i for i in K_LANTERN_SPEFFECTS if i in self.sp]
        self.counts["utility"] += self.track_entries(entries, self.UTILITY, True)
        return self

    # ---- classification consumers ----
    def is_enchant_effect(self, i):
        bits = self.src.get(i, 0)
        if bits & self.GREASE or i in self.enchant_vfx:
            return True
        r = self.sp.get(i)
        if not r:
            return False
        if int(_f(r, "spCategory")) > 0 and int(_f(r, "spCategory")) in self.enchant_categories:
            return True
        return self.has_enchant_vfx(r)

    def is_character_wide(self, i, always_persist):
        if i in always_persist:
            return True
        bits = self.src.get(i)
        if bits is None:
            return False
        if bits & self.GREASE:
            return False
        if not (bits & (self.SPELL | self.BEHAVIOR | self.CONSUM)):
            return False
        return not self.is_enchant_effect(i)


# ======================================================================
#  Support: names + parsing the hard-coded g_always_persist from config.hpp.
# ======================================================================
def load_names(names_path: Path):
    names = {}
    if not names_path or not Path(names_path).exists():
        return names
    for line in Path(names_path).read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if not line or " " not in line:
            continue
        head, _, rest = line.partition(" ")
        if head.isdigit():
            names[int(head)] = rest.strip()
    return names


def parse_always_persist(config_hpp: Path):
    """Pull the integer ids out of the g_always_persist initializer list."""
    if not Path(config_hpp).exists():
        return set()
    txt = Path(config_hpp).read_text(encoding="utf-8", errors="ignore")
    i = txt.find("g_always_persist")
    if i < 0:
        return set()
    brace = txt.find("{", i)
    end = txt.find("};", brace)
    if brace < 0 or end < 0:
        return set()
    body = txt[brace + 1:end]
    ids = set()
    for line in body.splitlines():
        code = line.split("//", 1)[0]  # drop trailing // comment (may hold numbers)
        num = ""
        for ch in code:
            if ch.isdigit():
                num += ch
            elif num:
                ids.add(int(num))
                num = ""
        if num:
            ids.add(int(num))
    return ids


def nm(names, i):
    return f"{i}:{names[i]}" if i in names else str(i)


# ======================================================================
#  CLI
# ======================================================================
def resolve_config(args):
    ini_path = TOOLS_DIR / "export_buffs.ini"
    cfg = configparser.ConfigParser()
    if ini_path.exists():
        cfg.read(ini_path)
    def get(key, default):
        if cfg.has_option("paths", key) and cfg.get("paths", key).strip():
            return cfg.get("paths", key).strip()
        return default
    reg = args.regulation or get("regulation", None)
    sf = args.soulsformats or get("soulsformats", str(DEFAULT_SOULSFORMATS))
    pdefs = args.paramdefs or get("paramdefs", str(DEFAULT_PARAMDEFS))
    names = args.names or get("names", str(DEFAULT_NAMES))
    return reg, Path(sf), Path(pdefs), Path(names)


def main():
    # UTF-8 stdout for the id:Name output (effect names carry non-ASCII).
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

    ap = argparse.ArgumentParser(description="Offline PersistentBuffs buff-discovery export.")
    ap.add_argument("--regulation", help="Path to regulation.bin (loose / mod).")
    ap.add_argument("--soulsformats", help="Path to Andre.SoulsFormats.dll.")
    ap.add_argument("--paramdefs", help="Dir of Paramdex ER/Defs *.xml.")
    ap.add_argument("--names", help="Paramdex SpEffectParam.txt for readable ids.")
    ap.add_argument("--json", help="Write the full result as JSON to this path.")
    ap.add_argument("--diff-always", action="store_true",
                    help="Diff the discovered character-wide set against the "
                         "hard-coded g_always_persist in src/config.hpp.")
    ap.add_argument("--check", action="store_true",
                    help="Plumbing self-test: load SoulsFormats + paramdefs and exit "
                         "(no regulation needed).")
    args = ap.parse_args()

    reg, sf_path, pdefs_path, names_path = resolve_config(args)

    if not sf_path.exists():
        sys.exit(f"[error] SoulsFormats DLL not found: {sf_path}\n"
                 f"        Pass --soulsformats or set it in tools/export_buffs.ini")

    print(f"[info] SoulsFormats: {sf_path}")
    sf = load_soulsformats(sf_path)
    defs = load_paramdefs(sf, pdefs_path)
    print(f"[info] paramdefs:    {pdefs_path}  ({len(defs)} loaded)")

    if args.check:
        need = ("SP_EFFECT_PARAM_ST", "EQUIP_PARAM_GOODS_ST", "MAGIC_PARAM_ST",
                "BULLET_PARAM_ST", "BEHAVIOR_PARAM_ST", "SP_EFFECT_VFX_PARAM_ST")
        missing = [p for p in need if p not in defs]
        print("[check] required paramdefs:",
              "ALL PRESENT" if not missing else f"MISSING {missing}")
        print("[check] plumbing OK")
        return

    if not reg:
        sys.exit("[error] no regulation.bin. Pass --regulation or set it in "
                 "tools/export_buffs.ini (see .example). Or run --check.")
    if not Path(reg).exists():
        sys.exit(f"[error] regulation not found: {reg}")

    print(f"[info] regulation:   {reg}")
    bnd = decrypt_regulation(sf, Path(reg))

    sp = param_to_dict(read_param(sf, bnd, "SpEffectParam", defs), SPEFFECT_FIELDS)
    goods = param_to_dict(read_param(sf, bnd, "EquipParamGoods", defs), GOODS_FIELDS)
    magic = param_to_dict(read_param(sf, bnd, "Magic", defs), MAGIC_FIELDS)
    bullet = param_to_dict(read_param(sf, bnd, "Bullet", defs), BULLET_FIELDS)
    beh_pc = param_to_dict(read_param(sf, bnd, "BehaviorParam_PC", defs), BEHAVIOR_FIELDS)
    beh = param_to_dict(read_param(sf, bnd, "BehaviorParam", defs), BEHAVIOR_FIELDS)
    vfx = param_to_dict(read_param(sf, bnd, "SpEffectVfxParam", defs), VFX_FIELDS)
    print(f"[info] rows: SpEffect={len(sp)} Goods={len(goods)} Magic={len(magic)} "
          f"Bullet={len(bullet)} BehaviorPC={len(beh_pc)} Behavior={len(beh)} Vfx={len(vfx)}")

    disc = Discovery(sp, goods, magic, bullet, beh_pc, beh, vfx).run()
    names = load_names(names_path)
    always = parse_always_persist(CONFIG_HPP)

    # is_persistable-equivalent: system ids are dropped even if tracked.
    persistable = {i for i in disc.tracked if not is_system_effect(i)}
    char_wide = sorted(i for i in persistable if disc.is_character_wide(i, always))
    enchant = sorted(i for i in persistable if disc.is_enchant_effect(i))

    c = disc.counts
    print()
    print(f"tracked {len(disc.tracked)} SpEffect(s) "
          f"[greases {c['grease']}, spells {c['spell']}, consumables {c['consum']} "
          f"(physick tears {c['physick']}), skills/AoW {c['skill']}, utility {c['utility']}]"
          f"  ({len(disc.enchant_categories)} enchant categories, "
          f"{len(disc.enchant_vfx)} enchant-vfx effects)")
    print(f"persistable (tracked minus system ids): {len(persistable)}")
    print(f"character-wide buffs: {len(char_wide)}   weapon-bound enchants: {len(enchant)}")

    if args.diff_always:
        char_wide_set = set(char_wide)
        print()
        print(f"--- g_always_persist diff (config.hpp has {len(always)} ids) ---")
        missing_from_code = sorted(char_wide_set - always)
        not_discovered = sorted(always - char_wide_set)
        print(f"discovered character-wide but NOT in g_always_persist "
              f"({len(missing_from_code)}) -- candidates to add / covered by discovery:")
        for i in missing_from_code:
            print("   ", nm(names, i))
        print(f"in g_always_persist but NOT discovered character-wide "
              f"({len(not_discovered)}) -- seed-only / enchant-classified:")
        for i in not_discovered:
            tag = " (enchant)" if disc.is_enchant_effect(i) else ""
            print("   ", nm(names, i) + tag)

    if args.json:
        out = {
            "counts": c,
            "enchant_categories": sorted(disc.enchant_categories),
            "tracked": sorted(disc.tracked),
            "persistable": sorted(persistable),
            "physick_tears": sorted(disc.physick),
            "character_wide": char_wide,
            "weapon_bound_enchants": enchant,
            "g_always_persist_in_code": sorted(always),
            "names": {str(i): names[i] for i in persistable if i in names},
        }
        Path(args.json).write_text(json.dumps(out, indent=2), encoding="utf-8")
        print(f"\n[info] wrote {args.json}")


if __name__ == "__main__":
    main()
