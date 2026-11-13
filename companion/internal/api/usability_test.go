package api

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"

	"bottie/companion/internal/models"
)

func TestSeekHTTPGuardsBoundsAndAcceptedFeedback(t *testing.T) {
	for _, scenario := range []string{"success", "zero", "track", "target", "missing", "null", "fraction", "duplicate", "pace", "switch"} {
		t.Run(scenario, func(t *testing.T) {
			h, _ := fixtureServer(t, "")
			w := request(h, "GET", "/v1/playback", "", "", "")
			var p models.Playback
			if json.Unmarshal(w.Body.Bytes(), &p) != nil || !p.Capabilities.Seek {
				t.Fatal("fixture seek missing")
			}
			value := "86400000"
			if scenario == "zero" {
				value = "0"
			}
			guard, track := p.ControlTarget, p.ControlTrack
			want := 200
			switch scenario {
			case "track":
				track = strings.Repeat("f", 32)
				want = 409
			case "target":
				guard = strings.Repeat("f", 32)
				want = 409
			case "null":
				value = "null"
				want = 400
			case "fraction":
				value = "1.0"
				want = 400
			case "missing", "duplicate":
				want = 400
			case "switch":
				request(h, "POST", "/v1/control", `{"action":"next"}`, "", "")
				want = 409
			}
			body := fmt.Sprintf(`{"action":"seek","value":%s,"control_target":%q,"control_track":%q}`, value, guard, track)
			if scenario == "missing" {
				body = fmt.Sprintf(`{"action":"seek","value":1,"control_target":%q}`, guard)
			}
			if scenario == "duplicate" {
				body = strings.TrimSuffix(body, "}") + fmt.Sprintf(`,"control_track":%q}`, track)
			}
			w = request(h, "POST", "/v1/control", body, "", "")
			if w.Code != want {
				t.Fatalf("status%d want%d", w.Code, want)
			}
			if want == 200 {
				var result models.ControlResult
				position := p.DurationMS - 1
				if scenario == "zero" {
					position = 0
				}
				if json.Unmarshal(w.Body.Bytes(), &result) != nil || !result.OK || result.Result != "accepted" || result.PositionMS == nil || *result.PositionMS != position {
					t.Fatal("accepted effective position missing")
				}
			}
			if scenario == "pace" {
				w = request(h, "POST", "/v1/control", body, "", "")
				if w.Code != 429 || w.Header().Get("Retry-After") != "1" {
					t.Fatal("seek write spacing")
				}
			}
		})
	}
}
