package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

var jpeg = []byte{0xff, 0xd8, 0xff, 0xe0, 'J', 'F', 'I', 'F', 0xff, 0xd9}

func post(t *testing.T, h http.Handler, body []byte, headers map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/upload", bytes.NewReader(body))
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func TestUploadSavesJPEGUnderDevice(t *testing.T) {
	dir := t.TempDir()
	h := newMux(dir, 1<<20)

	rec := post(t, h, jpeg, map[string]string{"X-Device-Id": "a1b2c3d4e5f6", "X-Trigger": "button"})
	if rec.Code != http.StatusCreated {
		t.Fatalf("status %d, body %q", rec.Code, rec.Body)
	}

	var resp struct {
		File  string `json:"file"`
		Bytes int    `json:"bytes"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	if filepath.Dir(resp.File) != "a1b2c3d4e5f6" || filepath.Ext(resp.File) != ".jpg" {
		t.Errorf("unexpected file path %q", resp.File)
	}
	if got, err := os.ReadFile(filepath.Join(dir, resp.File)); err != nil || !bytes.Equal(got, jpeg) {
		t.Errorf("saved file mismatch: %v", err)
	}
}

func TestUploadRejectsNonJPEG(t *testing.T) {
	rec := post(t, newMux(t.TempDir(), 1<<20), []byte("hello"), nil)
	if rec.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("status %d", rec.Code)
	}
}

func TestUploadRejectsOversizedBody(t *testing.T) {
	rec := post(t, newMux(t.TempDir(), 4), jpeg, nil)
	if rec.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("status %d", rec.Code)
	}
}

func TestDeviceIdCannotEscapeImageDir(t *testing.T) {
	dir := t.TempDir()
	rec := post(t, newMux(dir, 1<<20), jpeg, map[string]string{"X-Device-Id": "../../etc", "X-Trigger": "a/b"})
	if rec.Code != http.StatusCreated {
		t.Fatalf("status %d", rec.Code)
	}
	matches, _ := filepath.Glob(filepath.Join(dir, "*", "*.jpg"))
	if len(matches) != 1 {
		t.Fatalf("expected one image inside %s, found %v", dir, matches)
	}
}
