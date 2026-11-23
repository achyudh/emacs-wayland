;;; instructions.el --- Redisplay workloads counted by Callgrind -*- lexical-binding: t -*-

;; This file is part of GNU Emacs, as modified by the native Wayland
;; backend fork.
;; It is free software under the GNU General Public License, version 3
;; or later.

;;; Commentary:

;; run.py starts Emacs under Callgrind, counting only inside
;; `redisplay'.  The workload below is the only code that calls it, so
;; the count covers layout and drawing for a fixed sequence of
;; screens and nothing else.  Do not call `redisplay' or `sit-for'
;; anywhere else in this file.

;;; Code:

(defconst wayland-test-instructions-bytes 100000
  "How much of the source file the workload buffer holds.")

(defun wayland-test-instructions-buffer ()
  "Show a fontified copy of the start of the C source under test.
The copy keeps its faces with font lock off, so that redisplay runs
no Lisp and counts the same text on every run."
  (let ((text (with-temp-buffer
                (insert-file-contents (getenv "EMACS_WAYLAND_TEST_SOURCE")
                                      nil 0 wayland-test-instructions-bytes)
                (delay-mode-hooks (c-mode))
                (font-lock-ensure)
                (buffer-string))))
    (switch-to-buffer (get-buffer-create "*Wayland instructions*"))
    (font-lock-mode -1)
    ;; As `prog-mode' does; otherwise each line searches for the start
    ;; of its paragraph.
    (setq bidi-paragraph-direction 'left-to-right)
    (insert text)
    (goto-char (point-min))))

(defun wayland-test-instructions-repeat (count function)
  "Call FUNCTION, then redisplay, COUNT times.  Return COUNT.
A garbage collection inside a counted redisplay would add a tenth to
a workload, and where one falls changes with any edit to Emacs, so
none may run."
  (garbage-collect)
  (let ((gc-cons-threshold most-positive-fixnum)
        (gc-cons-percentage 1.0))
    (dotimes (_ count)
      (funcall function)
      (redisplay t)))
  count)

(ert-deftest wayland-test-redisplay-instructions ()
  (tool-bar-mode -1)
  (menu-bar-mode -1)
  (tab-bar-mode -1)
  (blink-cursor-mode -1)
  (set-frame-font "Monospace-14" nil (list wayland-test-frame))
  (wayland-test-instructions-buffer)
  ;; Reading an event redisplays from the command loop, which is not
  ;; counted: the frame takes its configured size and the font and
  ;; glyph caches fill before the workload starts.
  (read-event nil nil 1.0)
  (wayland-test-send "ready" (wayland-test-build-details))
  (wayland-test-send
   "done"
   ;; Line numbers make every row two glyph strings.  They also make
   ;; redisplay lay out the whole window after an edit, which would
   ;; hide the backend's share of the typing workload.
   (pcase (getenv "EMACS_WAYLAND_TEST_WORKLOAD")
     ("page" (display-line-numbers-mode 1)
      (wayland-test-instructions-repeat 40 #'scroll-up))
     ("line" (display-line-numbers-mode 1)
      (wayland-test-instructions-repeat 60 (lambda () (scroll-up 1))))
     ("typing" (forward-line 20)
      (end-of-line)
      (wayland-test-instructions-repeat 60 (lambda () (insert "x")))))))

;;; instructions.el ends here
