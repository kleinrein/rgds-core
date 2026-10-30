package main

import (
	"context"
	"os"
	"testing"
)

// RGDS_NET=1 go test -run Network: checks both ways of finding the latest release against GitHub.
func TestNetworkRelease(t *testing.T) {
	if os.Getenv("RGDS_NET") != "1" {
		t.Skip("set RGDS_NET=1")
	}
	api, err := fetchRelease(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	page, err := releaseFromRedirect(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if api.Version != page.Version || len(page.Images) != 2 {
		t.Fatalf("api %+v, page %+v", api, page)
	}
	for i := range page.Images {
		if page.Images[i].url != api.Images[i].url || page.Images[i].sumURL != api.Images[i].sumURL {
			t.Fatalf("%s: %s vs %s", page.Images[i].Name, page.Images[i].url, api.Images[i].url)
		}
	}
}
