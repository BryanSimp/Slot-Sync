"""Editing Dolphin.ini without wrecking it.

This module rewrites a file the user owns and did not ask us to reformat, so
what is under test is mostly what it leaves *alone*.
"""

from __future__ import annotations

from slotsync_dolphin.inifile import IniFile

SAMPLE = """[Analytics]
Enabled = False

[Core]
CPUThread = True
SlotA = 255
SkipIPL = True

[Display]
Fullscreen = False
"""


def write(tmp_path, text=SAMPLE):
    path = tmp_path / "Dolphin.ini"
    path.write_text(text, encoding="utf-8")
    return path


def test_reads_a_key(tmp_path):
    ini = IniFile(write(tmp_path))
    assert ini.get("Core", "SlotA") == "255"
    assert ini.get("Display", "Fullscreen") == "False"


def test_keys_are_scoped_to_their_section(tmp_path):
    ini = IniFile(write(tmp_path))
    assert ini.get("Analytics", "SlotA") is None
    assert ini.get("Nope", "SlotA") is None


def test_missing_key_and_missing_file(tmp_path):
    ini = IniFile(write(tmp_path))
    assert ini.get("Core", "NoSuchKey") is None
    assert IniFile(tmp_path / "absent.ini").get("Core", "SlotA") is None


def test_sections(tmp_path):
    assert IniFile(write(tmp_path)).sections() == ["Analytics", "Core", "Display"]


def test_setting_a_key_changes_only_that_line(tmp_path):
    path = write(tmp_path)
    ini = IniFile(path)
    assert ini.set("Core", "SlotA", "8") is True
    ini.save(backup=False)

    before = SAMPLE.splitlines()
    after = path.read_text(encoding="utf-8").splitlines()
    assert len(before) == len(after)
    differing = [i for i, (a, b) in enumerate(zip(before, after, strict=True)) if a != b]
    assert len(differing) == 1
    assert after[differing[0]] == "SlotA = 8"


def test_setting_an_identical_value_reports_no_change(tmp_path):
    """Callers use this to skip writing, and so skip taking a backup."""
    ini = IniFile(write(tmp_path))
    assert ini.set("Core", "SlotA", "255") is False


def test_an_absent_key_is_added_inside_its_section(tmp_path):
    path = write(tmp_path)
    ini = IniFile(path)
    ini.set("Core", "MemcardAPath", "C:/cards/GALE01.raw")
    ini.save(backup=False)

    lines = path.read_text(encoding="utf-8").splitlines()
    core = lines.index("[Core]")
    display = lines.index("[Display]")
    assert "MemcardAPath = C:/cards/GALE01.raw" in lines[core:display]
    # ...and it lands with the other keys, not after the blank separator.
    assert lines[display - 1] == ""
    assert lines[display - 2] == "MemcardAPath = C:/cards/GALE01.raw"


def test_an_absent_section_is_appended(tmp_path):
    path = write(tmp_path)
    ini = IniFile(path)
    ini.set("GBA", "Enabled", "True")
    ini.save(backup=False)

    text = path.read_text(encoding="utf-8")
    assert text.rstrip().endswith("[GBA]\nEnabled = True")
    assert "[Core]" in text  # nothing lost


def test_everything_not_touched_survives_byte_for_byte(tmp_path):
    path = write(tmp_path)
    ini = IniFile(path)
    ini.set("Core", "SlotA", "8")
    ini.save(backup=False)

    after = path.read_text(encoding="utf-8")
    for line in [
        "[Analytics]",
        "Enabled = False",
        "CPUThread = True",
        "SkipIPL = True",
        "[Display]",
        "Fullscreen = False",
    ]:
        assert line in after


def test_delimiter_spacing_is_preserved(tmp_path):
    path = write(tmp_path, "[Core]\nSlotA=255\nSlotB = 255\n")
    ini = IniFile(path)
    ini.set("Core", "SlotA", "8")
    ini.set("Core", "SlotB", "8")
    ini.save(backup=False)

    lines = path.read_text(encoding="utf-8").splitlines()
    assert "SlotA=8" in lines  # was tight
    assert "SlotB = 8" in lines  # was spaced


def test_a_backup_is_taken_once_and_never_overwritten(tmp_path):
    path = write(tmp_path)

    ini = IniFile(path)
    ini.set("Core", "SlotA", "8")
    backup = ini.save()
    assert backup is not None
    assert backup.read_text(encoding="utf-8") == SAMPLE

    # A second edit must not replace the pristine copy.
    ini2 = IniFile(path)
    ini2.set("Core", "SlotA", "9")
    assert ini2.save() is None
    assert backup.read_text(encoding="utf-8") == SAMPLE


def test_a_file_with_no_trailing_newline_still_round_trips(tmp_path):
    path = write(tmp_path, "[Core]\nSlotA = 255")
    ini = IniFile(path)
    ini.set("Core", "SlotA", "8")
    ini.save(backup=False)
    assert path.read_text(encoding="utf-8") == "[Core]\nSlotA = 8\n"


def test_non_ascii_paths_survive(tmp_path):
    """Card paths can contain anything a username can."""
    path = write(tmp_path)
    ini = IniFile(path)
    ini.set("Core", "MemcardAPath", "C:/Users/Ren\u00e9e/cards/GALE01.raw")
    ini.save(backup=False)
    assert "Ren\u00e9e" in IniFile(path).get("Core", "MemcardAPath")
