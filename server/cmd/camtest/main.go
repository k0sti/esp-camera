// Command camtest captures bursts of images at every resolution the camera
// supports, checks that the server saved valid JPEGs of the right size, and
// reports throughput.
//
//	go run ./cmd/camtest                         # upload test, 5 representative sizes
//	go run ./cmd/camtest -upload=false -sizes all # camera speed only, every size
//
// For each size it runs a burst without uploading (camera speed alone) and,
// unless -upload=false, a burst that uploads to the server (end to end).
// Exits non-zero if any capture, upload or saved file is wrong.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"image/jpeg"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"text/tabwriter"
	"time"
)

type size struct {
	Name   string `json:"name"`
	Width  int    `json:"width"`
	Height int    `json:"height"`
}

type burst struct {
	OK         bool   `json:"ok"`
	Size       string `json:"size"`
	Width      int    `json:"width"`
	Height     int    `json:"height"`
	Count      int    `json:"count"`
	Succeeded  int    `json:"succeeded"`
	BytesTotal int    `json:"bytes_total"`
	CaptureMs  int    `json:"capture_ms"`
	UploadMs   int    `json:"upload_ms"`
	ElapsedMs  int    `json:"elapsed_ms"`
}

func main() {
	board := flag.String("board", "http://192.168.1.82", "camera board base URL")
	imagesDir := flag.String("images", "../images", "directory the server saves images to")
	count := flag.Int("count", 3, "images per burst")
	only := flag.String("sizes", "QVGA,VGA,HD,UXGA,QXGA", `comma-separated sizes to test, or "all"`)
	doUpload := flag.Bool("upload", true, "also upload each burst and check the saved files")
	flag.Parse()

	sizes, err := fetchSizes(*board)
	if err != nil {
		fail("listing sizes: %v", err)
	}
	if *only != "all" {
		sizes = filterSizes(sizes, strings.Split(*only, ","))
	}

	var table strings.Builder
	tw := tabwriter.NewWriter(&table, 0, 0, 2, ' ', tabwriter.AlignRight)
	fmt.Fprintln(tw, "size\tresolution\tavg KB\tcamera fps\tupload ms/img\tend-to-end img/s\tupload KB/s\tlimited by\tfiles\t")

	failures := 0
	for i, s := range sizes {
		fmt.Printf("[%d/%d] %s...\n", i+1, len(sizes), s.Name)
		camera, err := runBurst(*board, s.Name, *count, false)
		var problems []string
		if err != nil {
			problems = append(problems, err.Error())
		}
		var upload burst
		files := 0
		if *doUpload {
			started := time.Now()
			upload, err = runBurst(*board, s.Name, *count, true)
			if err != nil {
				problems = append(problems, err.Error())
			}
			if files, err = checkFiles(*imagesDir, s, started); err != nil {
				problems = append(problems, err.Error())
			}
			if files != *count {
				problems = append(problems, fmt.Sprintf("expected %d saved files, found %d", *count, files))
			}
		}

		fmt.Fprintf(tw, "%s\t%dx%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t\n", s.Name, s.Width, s.Height,
			avgKB(camera), rate(camera.Succeeded, camera.ElapsedMs),
			perImage(upload.UploadMs, upload.Succeeded), rate(upload.Succeeded, upload.ElapsedMs),
			kbPerSec(upload.BytesTotal, upload.UploadMs), limitedBy(camera, upload), filesCell(*doUpload, files, *count))
		for _, p := range problems {
			fmt.Printf("  FAIL %s: %s\n", s.Name, p)
		}
		if len(problems) > 0 {
			failures++
		}
	}

	tw.Flush()
	fmt.Print("\n" + table.String())
	if failures > 0 {
		fail("%d of %d sizes failed", failures, len(sizes))
	}
	fmt.Printf("all %d sizes passed\n", len(sizes))
}

func fetchSizes(board string) ([]size, error) {
	resp, err := http.Get(board + "/sizes")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	var body struct {
		Sizes []size `json:"sizes"`
	}
	return body.Sizes, json.NewDecoder(resp.Body).Decode(&body)
}

func filterSizes(all []size, names []string) []size {
	var out []size
	for _, n := range names {
		for _, s := range all {
			if strings.EqualFold(s.Name, strings.TrimSpace(n)) {
				out = append(out, s)
			}
		}
	}
	return out
}

func runBurst(board, name string, count int, upload bool) (burst, error) {
	q := url.Values{"size": {name}, "count": {fmt.Sprint(count)}, "upload": {"0"}}
	if upload {
		q.Set("upload", "1")
	}
	client := http.Client{Timeout: time.Duration(count)*15*time.Second + 30*time.Second}
	resp, err := client.Get(board + "/burst?" + q.Encode())
	if err != nil {
		return burst{}, err
	}
	defer resp.Body.Close()
	var b burst
	if err := json.NewDecoder(resp.Body).Decode(&b); err != nil {
		return b, fmt.Errorf("burst upload=%v: HTTP %d: %v", upload, resp.StatusCode, err)
	}
	if !b.OK {
		return b, fmt.Errorf("burst upload=%v: %d/%d succeeded", upload, b.Succeeded, b.Count)
	}
	return b, nil
}

// checkFiles counts images saved for size s since `since` and verifies each
// one decodes as a JPEG of the expected resolution.
func checkFiles(dir string, s size, since time.Time) (int, error) {
	paths, err := filepath.Glob(filepath.Join(dir, "*", "*_test-"+s.Name+".jpg"))
	if err != nil {
		return 0, err
	}
	n := 0
	for _, p := range paths {
		info, err := os.Stat(p)
		if err != nil || info.ModTime().Before(since) {
			continue
		}
		n++
		f, err := os.Open(p)
		if err != nil {
			return n, err
		}
		cfg, err := jpeg.DecodeConfig(f)
		f.Close()
		if err != nil {
			return n, fmt.Errorf("%s: %v", filepath.Base(p), err)
		}
		if cfg.Width != s.Width || cfg.Height != s.Height {
			return n, fmt.Errorf("%s is %dx%d", filepath.Base(p), cfg.Width, cfg.Height)
		}
	}
	return n, nil
}

// limitedBy compares the time per image of the camera alone with the time
// per upload to say which one caps end-to-end throughput.
func limitedBy(camera, upload burst) string {
	if camera.Succeeded == 0 || upload.Succeeded == 0 {
		return "-"
	}
	cameraMs := float64(camera.ElapsedMs) / float64(camera.Succeeded)
	uploadMs := float64(upload.UploadMs) / float64(upload.Succeeded)
	if cameraMs > uploadMs {
		return "camera"
	}
	return "upload"
}

func filesCell(uploaded bool, files, count int) string {
	if !uploaded {
		return "-"
	}
	return fmt.Sprintf("%d/%d", files, count)
}

func rate(n, ms int) string {
	if ms == 0 {
		return "-"
	}
	return fmt.Sprintf("%.1f", float64(n)*1000/float64(ms))
}

func perImage(ms, n int) string {
	if n == 0 {
		return "-"
	}
	return fmt.Sprint(ms / n)
}

func avgKB(b burst) string {
	if b.Succeeded == 0 {
		return "-"
	}
	return fmt.Sprintf("%.0f", float64(b.BytesTotal)/float64(b.Succeeded)/1024)
}

func kbPerSec(bytes, ms int) string {
	if ms == 0 {
		return "-"
	}
	return fmt.Sprintf("%.0f", float64(bytes)/1024*1000/float64(ms))
}

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "camtest: "+format+"\n", args...)
	os.Exit(1)
}
