package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"time"
)

// runWorker is the elevated half (macOS, Windows, Linux): it writes the card and posts its progress to the UI process.
func runWorker(report, disk, image string) int {
	post := func(u update) {
		body, _ := json.Marshal(u)
		for try := 0; try < 3; try++ {
			resp, err := http.Post(report, "application/json", bytes.NewReader(body))
			if err == nil {
				resp.Body.Close()
				return
			}
			time.Sleep(200 * time.Millisecond)
		}
	}
	var last time.Time
	err := flash(image, disk, func(phase string, done, total int64) {
		if phase != "write" && phase != "verify" || time.Since(last) > 150*time.Millisecond || done == total {
			last = time.Now()
			post(update{Phase: phase, Done: done, Total: total})
		}
	})
	if err != nil {
		post(update{Phase: "error", Error: err.Error()})
		return 1
	}
	post(update{Phase: "done"})
	return 0
}
