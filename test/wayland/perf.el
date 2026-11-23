;;; perf.el --- GUI benchmark workloads run by run.py -*- lexical-binding: t -*-

;; This file is part of GNU Emacs, as modified by the native Wayland
;; backend fork.
;; It is free software under the GNU General Public License, version 3
;; or later.

;;; Code:

(require 'image)

(defvar wayland-test-perf-init-loaded)

(defun wayland-test-perf-image-buffer ()
  (let* ((directory (expand-file-name "images/" data-directory))
         (png (create-image (expand-file-name "splash.png" directory)
                           'png nil :width 320 :height 240))
         (svg (create-image (expand-file-name "splash.svg" directory)
                           'svg nil :width 320 :height 240)))
    (fundamental-mode)
    (setq-local cursor-type nil mode-line-format nil truncate-lines t)
    (erase-buffer)
    (dotimes (i 128)
      (insert (propertize "#" 'display (if (zerop (% i 2)) png svg)) "\n"))
    (goto-char (point-min))))

(defun wayland-test-perf-frame-size ()
  (vector (frame-pixel-width) (frame-pixel-height)
          (frame-char-height)))

(defun wayland-test-perf-ready ()
  (wayland-test-send "ready"
                     (append (wayland-test-build-details)
                             (list :init_ms (* 1000 (float-time
                                                    (time-subtract
                                                     after-init-time
                                                     before-init-time)))))))

(defun wayland-test-perf-resize ()
  "Collect resize samples while the normal command loop runs."
  (wayland-test-text-frame)
  (let (samples warming-up)
    (add-hook 'window-size-change-functions
              (lambda (window)
                (when (eq (window-frame window) wayland-test-frame)
                  (let ((size (with-selected-window window
                                (wayland-test-perf-frame-size))))
                    (if warming-up
                        (progn
                          (setq warming-up nil)
                          (wayland-test-send "resize-warmed" size))
                      (push size samples)))))
              nil t)
    (redisplay t)
    (set-process-filter
     wayland-test-connection
     (lambda (process text)
       (wayland-test-filter process text)
       (while (string-match-p "\n" wayland-test-input)
         (pcase (wayland-test-read-command)
           ("resize-arm"
            (setq warming-up t)
            (wayland-test-send "resize-armed"))
           ("resize-samples"
            (wayland-test-send "resize-samples" (vconcat (nreverse samples)))
            (setq samples nil))
           ("done" (run-at-time 0 nil #'kill-emacs 0))
           (command (error "Unknown resize command %S" command))))))
    (wayland-test-perf-ready)))

(defun wayland-test-perf-workload (test)
  (let ((canvas nil)
        (canvas-pixels nil)
        (payload nil)
        (selection-key nil)
        (frames nil))
    (pcase test
      ("redraw-perf" (wayland-test-benchmark-buffer))
      ("canvas-perf"
       (wayland-test-text-frame)
       (setq canvas-pixels (vector (make-vector (* 512 512) #xff2468b1)
                                  (make-vector (* 512 512) #xffbe4927))
             canvas (list 'image :type 'canvas :id 'wayland-test-perf
                          :data-width 512 :data-height 512
                          :data (aref canvas-pixels 0)))
       (insert (propertize "#" 'display canvas)))
      ((or "startup-perf" "startup-init-perf")
       (when (equal test "startup-init-perf")
         (should (bound-and-true-p wayland-test-perf-init-loaded)))
       ;; The runner times the first capture of this color as Emacs's
       ;; first content, after anything the server or toolkit paints.
       (set-face-background 'default "#3a6ea5" wayland-test-frame))
      (_
       (wayland-test-text-frame)
       (insert (make-string 100 ?W) "\n")))
    (redisplay t)
    (wayland-test-perf-ready)
    (catch 'done
      (while t
        (when (member test '("startup-perf" "startup-init-perf"))
          ;; Keep redisplaying while the runner waits for the startup marker.
          (while (not (string-match-p "\n" wayland-test-input))
            (accept-process-output nil 0.01)
            (redisplay t)))
        (when (equal test "clipboard-perf")
          ;; X serves selection events from the input loop.  The peer
          ;; can ask for TARGETS before the runner sends another command.
          (while (not (string-match-p "\n" wayland-test-input))
            (when (eq (read-event nil nil 0.02) ?a)
              (setq selection-key t))))
        (pcase (split-string (wayland-test-read-command) " " t)
          ('("done") (throw 'done nil))
          ('("perf-state")
           (wayland-test-send "perf-state" (wayland-test-perf-state)))
          ('("trace-start")
           (wayland--trace wayland-test-terminal t)
           (wayland-test-send "trace-start"))
          ('("trace")
           (wayland-test-send "trace" (wayland--trace wayland-test-terminal)))
          ('("geometry")
           (redisplay t)
           (wayland-test-send "geometry" (wayland-test-perf-frame-size)))
          ('("scale-mark")
           (set-background-color "#077b3e")
           (redraw-frame)
           (redisplay t)
           (wayland-test-send "scale-mark"))
          (`("redraw" ,count)
           (dotimes (i (string-to-number count))
             (set-window-start nil
                               (save-excursion
                                 (goto-char (point-min))
                                 (forward-line (* (% (1+ i) 2) 500))
                                 (point)))
             (redraw-frame)
             (redisplay t)
             (sleep-for 0.1))
           (wayland-test-send "workload-done"))
          (`("canvas" ,count)
           (dotimes (i (string-to-number count))
             (setcar (cdr (memq :data canvas))
                     (aref canvas-pixels (% (1+ i) 2)))
             (canvas-refresh canvas t)
             (redisplay t)
             (sleep-for (/ 1.0 60)))
           (wayland-test-send "workload-done"))
          ('("yank")
           (erase-buffer)
           ;; Time the yank here: the wait for the command polls.
           (let ((kill-ring nil) (last-command nil) (start (float-time)))
             (yank)
             (setq payload (buffer-string))
             (wayland-test-send "yanked"
                                (vector (string-bytes payload)
                                        (* 1000 (- (float-time) start)))))
           (erase-buffer))
          ('("hash")
           (wayland-test-send "hash" (secure-hash 'sha256 payload))
           (setq payload nil))
          ('("selection-key")
           (should (or selection-key (eq (read-event nil nil 2) ?a)))
           (setq selection-key nil)
           (when (eq window-system 'wayland)
             (should (wayland--selection-publishable-p nil)))
           (wayland-test-send "selection-key"))
          (`("publish" ,size)
           (let ((text (with-temp-buffer
                         (insert-file-contents-literally
                          (expand-file-name size
                                            (getenv "EMACS_WAYLAND_TEST_DATA")))
                         (buffer-string))))
             (gui-set-selection 'CLIPBOARD text)
             (should (gui-backend-selection-owner-p 'CLIPBOARD)))
           (wayland-test-send "published"))
          (`("frame" ,name)
           (let ((frame (make-frame `((name . ,name) (title . ,name)
                                      (width . 40) (height . 14)))))
             (push frame frames)
             (redisplay t)
             (wayland-test-send "frame")))
          (command (error "Unknown benchmark command %S" command)))))
    (dolist (frame frames)
      (delete-frame frame))
    (kill-emacs 0)))

;;; perf.el ends here
