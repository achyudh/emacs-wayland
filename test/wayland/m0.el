;;; m0.el --- Wayland transport probes run by run.py -*- lexical-binding: t -*-

;; This file is part of GNU Emacs, as modified by the native Wayland
;; backend fork.
;; It is free software under the GNU General Public License, version 3
;; or later.

;;; Code:

(require 'ert)
(require 'json)

(defvar wayland-test-connection nil)
(defvar wayland-test-input "")
(defvar wayland-test-frame nil)
(defvar wayland-test-terminal nil)

(defun wayland-test-send (phase &optional data)
  (process-send-string wayland-test-connection
                       (concat (json-serialize `(:phase ,phase :data ,data)) "\n")))

(defun wayland-test-filter (_process string)
  (setq wayland-test-input (concat wayland-test-input string)))

(defun wayland-test-read-command (&optional process)
  (while (not (string-match "\n" wayland-test-input))
    (should (process-live-p wayland-test-connection))
    (accept-process-output process 0.05))
  (let ((end (match-beginning 0)))
    (prog1 (substring wayland-test-input 0 end)
      (setq wayland-test-input (substring wayland-test-input (1+ end))))))

(defun wayland-test-next (command &optional process)
  (should (equal command (wayland-test-read-command process))))

(defun wayland-test-wait (predicate &optional process)
  (while (not (funcall predicate))
    (should (process-live-p wayland-test-connection))
    (accept-process-output process 0.05)))

(defun wayland-test-metrics (&optional key)
  (let ((metrics (wayland--metrics wayland-test-terminal)))
    (if key (plist-get metrics key) metrics)))

(defun wayland-test-counter-report ()
  (let ((metrics (wayland-test-metrics)) result)
    (dolist (key '(:hook-calls :service-calls :max-hook-ns :pings :raw-keys
                   :flush-eagain :flush-epipe :output-resumed :write-resumes
                   :finalize-deferrals))
      (setq result (plist-put result key (plist-get metrics key))))
    result))

(defun wayland-test-survivor ()
  (let* ((baud-rate 9600)
         (frame (make-frame-on-display
                 (getenv "EMACS_WAYLAND_TEST_SURVIVOR"))))
    (should (= baud-rate 9600))
    (should (eq (window-system frame) 'wayland))
    (with-selected-frame frame
      (force-window-update t)
      (redisplay t))
    (select-frame wayland-test-frame)
    frame))

(defun wayland-test-after-loss (survivor)
  (wayland-test-wait (lambda () (not (frame-live-p wayland-test-frame))))
  (should-not (terminal-live-p wayland-test-terminal))
  (should (frame-live-p survivor))
  (should (display-graphic-p survivor))
  (with-selected-frame survivor
    (force-window-update t)
    (redisplay t))
  (garbage-collect))

(ert-deftest wayland-test-startup ()
  (should (eq (window-system wayland-test-frame) 'wayland))
  (should (display-graphic-p wayland-test-frame))
  (should (equal (getenv "WAYLAND_DISPLAY")
                 (frame-parameter wayland-test-frame 'display)))
  (should (> (display-pixel-width wayland-test-frame) 0))
  ;; A slow terminal changes isearch, among others.
  (should (> baud-rate search-slow-speed))
  ;; Shared Lisp asks these before treating a frame as graphical.
  ;; Without symbol keys, `normal-erase-is-backspace-mode' translates
  ;; C-h to DEL and the Delete key deletes backward, as on a terminal.
  (should (display-symbol-keys-p wayland-test-frame))
  (should (equal (lookup-key local-function-key-map [delete]) [deletechar]))
  ;; `display-mouse-p' follows the seat, which has no pointer in a
  ;; headless sway: `seat-changes' checks it both ways.
  (should (display-selections-p wayland-test-frame))
  (should (display-grayscale-p wayland-test-frame))
  ;; Init files commonly turn scroll bars off; there are none, but the
  ;; mode must exist and be off.
  (should (fboundp 'scroll-bar-mode))
  (should-not scroll-bar-mode)
  (should-not (frame-parameter wayland-test-frame 'vertical-scroll-bars))
  (wayland-test-send "ready")
  (wayland-test-next "done"))

(ert-deftest wayland-test-startup-background ()
  ;; The runner's init file stayed in Lisp until a configure was
  ;; answered with the background.  Sway may send its tiled size
  ;; before the first image, which then replaces the background for
  ;; good.
  (should (bound-and-true-p wayland-test-early-background))
  (redisplay t)
  (let ((commits (wayland-test-metrics :background-commits)))
    (wayland-test-send "ready")
    (wayland-test-next "done")
    (should (= (wayland-test-metrics :background-commits) commits))))

(ert-deftest wayland-test-quiet-ping ()
  ;; SIGIO would run the input hook and bypass the path under test.
  (wayland--debug-sigio nil wayland-test-terminal)
  (let ((quiet (make-pipe-process :name "wayland-quiet" :noquery t))
        (before (wayland-test-counter-report)))
    (unwind-protect
        (progn
          (wayland-test-send "ready")
          (wayland-test-next "done" quiet)
          (should (> (wayland-test-metrics :service-calls)
                     (plist-get before :service-calls)))
          (should (= (wayland-test-metrics :hook-calls)
                     (plist-get before :hook-calls)))
          (should (= (wayland-test-metrics :pings) 1))
          (wayland-test-send "metrics" (wayland-test-counter-report)))
      (delete-process quiet))))

(ert-deftest wayland-test-busy-lisp ()
  (wayland-test-send "ready")
  (wayland-test-next "go")
  (let ((deadline (+ (float-time) 5)) (iterations 0)
        (pings (wayland-test-metrics :pings))
        (hook-calls (wayland-test-metrics :hook-calls)))
    (wayland-test-send "busy")
    ;; The input timer can arrange for the evaluator's safe points to
    ;; run the hook.  This loop never enters a process wait.
    (while (and (= (wayland-test-metrics :pings) pings)
                (< (float-time) deadline))
      (setq iterations (1+ iterations)))
    (should (> iterations 0))
    (should (> (wayland-test-metrics :pings) pings))
    (should (> (wayland-test-metrics :hook-calls) hook-calls))
    (wayland-test-send "metrics" (wayland-test-counter-report))
    (wayland-test-next "done")))

(defun wayland-test-backpressure (with-input)
  (tool-bar-mode -1)
  (redisplay t)
  (should (wayland-test-metrics :keymap))
  (when with-input (wayland--debug-selection wayland-test-terminal))
  (wayland-test-send "ready")
  (wayland-test-next "fill")
  (let ((resumes (wayland-test-metrics :output-resumed))
        (write-resumes (wayland-test-metrics :write-resumes))
        (hook-calls (wayland-test-metrics :hook-calls)))
    (should (eq (wayland--debug-request-burst 50000 nil wayland-test-terminal)
                'pending))
    (when with-input
      ;; Let the periodic input hook run with the socket still full.
      ;; Busy evaluation keeps the service fd out of this interval.
      (while (= (wayland-test-metrics :hook-calls) hook-calls))
      (should (eq (wayland-test-metrics :output) 'pending))
      (should (> (wayland-test-metrics :max-hook-ns) 0)))
    (wayland-test-send "pending" (wayland-test-counter-report))
    (wayland-test-next "draw")
    ;; Settle the configure and draw with output still blocked.  The
    ;; runner checks that the new image reaches the server only after
    ;; draining the socket makes the write descriptor ready.
    (while (or (/= (frame-pixel-width wayland-test-frame) 800)
               (/= (frame-pixel-height wayland-test-frame) 600))
      (redisplay t)
      (accept-process-output nil 0.02))
    (redisplay t)
    (should (eq (wayland-test-metrics :output) 'pending))
    (when with-input
      (should (= (wayland-test-metrics :pings) 1))
      (should (= (alist-get 30 (wayland-test-metrics :raw-keycodes)) 2)))
    (wayland-test-send "held" (vector (frame-pixel-width wayland-test-frame)
                                     (frame-pixel-height wayland-test-frame)))
    (wayland-test-next "done")
    (should (eq (wayland-test-metrics :output) 'idle))
    (should (> (wayland-test-metrics :output-resumed) resumes))
    (unless with-input
      (should (> (wayland-test-metrics :write-resumes) write-resumes)))
    (wayland-test-send "metrics" (wayland-test-counter-report))))

(ert-deftest wayland-test-backpressure-input ()
  (wayland-test-backpressure t))

(ert-deftest wayland-test-write-only ()
  ;; SIGIO would run the input hook and bypass the path under test.
  (wayland--debug-sigio nil wayland-test-terminal)
  (wayland-test-backpressure nil))

(defun wayland-test-thread-wait (wait)
  (let ((thread
         (make-thread
          (lambda ()
            (let ((quiet (make-pipe-process :name "wayland-thread" :noquery t)))
              (unwind-protect
                  (progn
                    (wayland-test-send "waiting")
                    (funcall wait quiet)
                    t)
                (delete-process quiet)))))))
    (should (thread-join thread))))

(ert-deftest wayland-test-thread-selection ()
  (wayland-test-send "ready"
                     (vconcat (wayland--dump-frame nil wayland-test-frame)))
  (wayland-test-next "publish")
  (should (eq (read-event nil nil 2) ?a))
  (gui-set-selection 'CLIPBOARD "emacs-wayland-m0")
  (wayland--debug-sigio nil wayland-test-terminal)
  (wayland-test-send "published")
  (wayland-test-next "go")
  (wayland-test-thread-wait (lambda (quiet) (wayland-test-next "done" quiet))))

(ert-deftest wayland-test-thread-loss ()
  (let ((survivor (wayland-test-survivor)))
    (wayland-test-send "ready")
    (wayland-test-next "go")
    (wayland-test-thread-wait
     (lambda (quiet)
       (wayland-test-wait (lambda () (not (frame-live-p wayland-test-frame)))
                          quiet)))
    (wayland-test-after-loss survivor)))

(defun wayland-test-pre-redisplay-loss (queued-quit)
  (let ((survivor (wayland-test-survivor)) reached)
    (wayland-test-send "ready")
    (wayland-test-next "go")
    (let ((pre-redisplay-function
           (lambda (_frames)
             (unless reached
               (setq reached t)
               (let ((inhibit-quit t) (quit-flag queued-quit))
                 (wayland-test-send "redisplaying")
                 (wayland-test-wait
                  (lambda ()
                    (and (eq (wayland-test-metrics :state) 'dead)
                         (> (wayland-test-metrics :finalize-deferrals) 0))))
                 (should (frame-live-p wayland-test-frame))
                 (wayland-test-send "guarded" (wayland-test-counter-report))
                 (wayland-test-next "leave")
                 (should (frame-live-p wayland-test-frame))
                 (when queued-quit (should quit-flag))
                 (wayland-test-send "leaving"))))))
      (force-window-update t)
      (redisplay t))
    (should reached)
    (wayland-test-after-loss survivor)))

(ert-deftest wayland-test-pre-redisplay-loss ()
  (wayland-test-pre-redisplay-loss nil))

(ert-deftest wayland-test-queued-quit ()
  (wayland-test-pre-redisplay-loss t))

(ert-deftest wayland-test-redisplay-loss ()
  ;; SIGIO on either display can run the primary's input hook before
  ;; the glyph draw, even while redisplay calculates the mode line.
  (wayland--debug-sigio nil wayland-test-terminal)
  (let ((survivor (wayland-test-survivor)) armed)
    (wayland--debug-sigio nil (frame-terminal survivor))
    ;; Send the first image so its withheld callback paces later commits.
    (redisplay t)
    (wayland-test-send "ready")
    (wayland-test-next "draw")
    ;; A forced update alone can reuse the existing glyph rows.
    (erase-buffer)
    (insert "New glyphs for the disconnect probe")
    (goto-char (point-min))
    (let ((pre-redisplay-function
           (lambda (_frames)
             (unless armed
               (setq armed t)
               (wayland-test-send "drawing-next")
               ;; The runner disconnects the primary before releasing
               ;; this fixture.  Stay out of the process wait so that
               ;; the next glyph draw is where dispatch sees the loss.
               (while (not (file-exists-p (getenv "EMACS_WAYLAND_TEST_RESUME"))))
               (should (eq (wayland-test-metrics :state) 'live))
               (wayland--debug-dispatch-after-draw 1 wayland-test-terminal)))))
      (force-window-update t)
      (redisplay t))
    (should armed)
    (wayland-test-after-loss survivor)))

(ert-deftest wayland-test-epipe ()
  ;; SIGIO would run the input hook and bypass the path under test.
  (wayland--debug-sigio nil wayland-test-terminal)
  (let ((survivor (wayland-test-survivor)) reached)
    (wayland-test-send "ready")
    (wayland-test-next "go")
    (let ((pre-redisplay-function
           (lambda (_frames)
             (unless reached
               (setq reached t)
               (wayland-test-send "flushing")
               ;; A file rendezvous here avoids entering a wait that
               ;; would read the pending protocol error before flushing.
               (while (not (file-exists-p (getenv "EMACS_WAYLAND_TEST_RESUME"))))
               (wayland--debug-request-burst 1 nil wayland-test-terminal)
               (should (= (wayland-test-metrics :flush-epipe) 1))
               (should (eq (wayland-test-metrics :state) 'dead))
               (wayland-test-send "metrics" (wayland-test-counter-report))))))
      (force-window-update t)
      (redisplay t))
    (should reached)
    (wayland-test-after-loss survivor)))

(ert-deftest wayland-test-overflow ()
  (let ((survivor (wayland-test-survivor)))
    (wayland-test-send "ready")
    (wayland-test-next "fill")
    ;; 200,000 region additions need 4.8 MiB, beyond the 4 MiB cap.
    (wayland--debug-request-burst 200000 nil wayland-test-terminal)
    (wayland-test-after-loss survivor)))

(defun wayland-test-log-result (event &rest args)
  (when (eq event 'test-ended)
    (pcase-let ((`(,_ ,test ,result) args))
      (unless (ert-test-result-expected-p test result)
        (princ (format "%S: %S\n" (ert-test-name test)
                       (if (ert-test-result-with-condition-p result)
                           (ert-test-result-with-condition-condition result)
                         result))
               'external-debugging-output)))))

(defun wayland-test-run ()
  (let* ((test (getenv "EMACS_WAYLAND_TEST"))
         (poll-input (member test '("busy-lisp" "backpressure-input"))))
    (setq polling-period (if poll-input 0.05 1000.0)
          wayland-test-frame (selected-frame)
          wayland-test-terminal (frame-terminal wayland-test-frame)
          wayland-test-connection
          (make-network-process :name "wayland-test-runner" :family 'local
                                :service (getenv "EMACS_WAYLAND_TEST_SOCKET")
                                :coding 'utf-8-unix :noquery t
                                :filter #'wayland-test-filter))
    (when poll-input
      (set-input-interrupt-mode nil))
    (when (and (eq window-system 'wayland)
               (getenv "EMACS_WAYLAND_TEST_PRESENTATION"))
      (wayland--presentation-feedback t wayland-test-terminal))
    (cond
     ((equal test "idle-loss")
      ;; Let the command loop idle with one frame.  The runner checks
      ;; that losing the last display exits cleanly with status 70.
      (wayland-test-send "ready"))
     ((member test '("observer-emacs" "typing-perf"))
      ;; Typing must go through the command loop.
      (wayland-test-observer (equal test "typing-perf")))
     ((member test '("scroll-perf" "scroll-cadence" "image-scroll-perf"))
      (wayland-test-scroll-benchmark))
     ((equal test "resize-perf")
      (wayland-test-perf-resize))
     ((fboundp 'wayland-test-perf-workload)
      (wayland-test-perf-workload test))
     ((member test '("soak-storm" "soak-sway"))
      (wayland-test-soak))
     (t
      (let ((stats (ert-run-tests (intern (concat "wayland-test-" test))
                                  #'wayland-test-log-result)))
        (kill-emacs (if (zerop (ert-stats-completed-unexpected stats))
                        0 1)))))))

;;; m0.el ends here
