//go:build darwin

package main

import "testing"

func TestParsePlist(t *testing.T) {
	v, err := parsePlist([]byte(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>AllDisksAndPartitions</key><array><dict><key>Partitions</key><array>
		<dict><key>VolumeName</key><string>RGDS-BOOT</string><key>MountPoint</key><string>/Volumes/RGDS-BOOT</string></dict>
		<dict><key>Content</key><string>Linux Filesystem</string></dict>
	</array></dict></array>
	<key>Empty</key><array/>
	<key>Internal</key><false/><key>RemovableMedia</key><true/>
	<key>TotalSize</key><integer>31914983424</integer>
	<key>WholeDisks</key><array><string>disk4</string></array>
</dict></plist>`))
	if err != nil {
		t.Fatal(err)
	}
	m := v.(map[string]any)
	if boolKey(m, "Internal") || !boolKey(m, "RemovableMedia") || num(m, "TotalSize") != 31914983424 {
		t.Fatalf("%v", m)
	}
	if w := array(m["WholeDisks"]); len(w) != 1 || w[0] != "disk4" {
		t.Fatalf("WholeDisks %v", w)
	}
	parts := array(array(m["AllDisksAndPartitions"])[0].(map[string]any)["Partitions"])
	if len(parts) != 2 || str(parts[0].(map[string]any), "VolumeName") != "RGDS-BOOT" {
		t.Fatalf("partitions %v", parts)
	}
}
