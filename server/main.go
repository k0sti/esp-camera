// Command server receives JPEG uploads from the ESP32-S3 camera and saves
// them as <dir>/<device>/<timestamp>_<trigger>.jpg.
//
//	POST /upload    body: image/jpeg; headers X-Device-Id, X-Trigger
//	GET  /images/   browse saved images
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"time"
)

func main() {
	addr := flag.String("addr", ":8080", "listen address")
	dir := flag.String("dir", "images", "directory to save images in")
	maxSize := flag.Int64("max-size", 10<<20, "maximum upload size in bytes")
	flag.Parse()

	if err := os.MkdirAll(*dir, 0o755); err != nil {
		log.Fatal(err)
	}
	log.Printf("listening on %s, saving images to %s", *addr, *dir)
	log.Fatal(http.ListenAndServe(*addr, newMux(*dir, *maxSize)))
}

func newMux(dir string, maxSize int64) *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /upload", uploadHandler(dir, maxSize))
	mux.Handle("GET /images/", http.StripPrefix("/images/", http.FileServer(http.Dir(dir))))
	return mux
}

var unsafeChars = regexp.MustCompile(`[^A-Za-z0-9_-]`)

// pathSafe makes a header value usable as a single path element.
func pathSafe(s, fallback string) string {
	s = unsafeChars.ReplaceAllString(s, "")
	if len(s) > 32 {
		s = s[:32]
	}
	if s == "" {
		return fallback
	}
	return s
}

func uploadHandler(dir string, maxSize int64) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		data, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxSize))
		var tooLarge *http.MaxBytesError
		switch {
		case errors.As(err, &tooLarge):
			http.Error(w, "image too large", http.StatusRequestEntityTooLarge)
			return
		case err != nil:
			http.Error(w, "reading body: "+err.Error(), http.StatusBadRequest)
			return
		case !bytes.HasPrefix(data, []byte{0xff, 0xd8}):
			http.Error(w, "body is not a JPEG", http.StatusUnsupportedMediaType)
			return
		}

		device := pathSafe(r.Header.Get("X-Device-Id"), "unknown")
		trigger := pathSafe(r.Header.Get("X-Trigger"), "unknown")
		name := fmt.Sprintf("%s_%s.jpg", time.Now().Format("20060102-150405.000"), trigger)
		rel := filepath.Join(device, name)

		if err := saveFile(filepath.Join(dir, rel), data); err != nil {
			log.Printf("saving %s: %v", rel, err)
			http.Error(w, "could not save image", http.StatusInternalServerError)
			return
		}
		log.Printf("saved %s (%d bytes) from %s", rel, len(data), r.RemoteAddr)

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusCreated)
		json.NewEncoder(w).Encode(map[string]any{"file": rel, "bytes": len(data)})
	}
}

// saveFile writes via a temp file so a half-written image never appears.
func saveFile(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".upload-*")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmp.Name(), path)
}
