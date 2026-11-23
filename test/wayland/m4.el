;;; m4.el --- Wayland M4 probes run by run.py -*- lexical-binding: t -*-

;; This file is part of GNU Emacs, as modified by the native Wayland
;; backend fork.
;; It is free software under the GNU General Public License, version 3
;; or later.

;;; Code:

(require 'ert)
(require 'dbus)

(defvar wayland-test-portal
  '(("monospace-font-name" . "DejaVu Sans Mono 12")
    ("font-name" . "DejaVu Sans 11")
    ("text-scaling-factor" . 1.0))
  "The settings the test's portal serves.")

(defun wayland-test-portal-read-all (&rest _)
  "Answer the settings portal's ReadAll from `wayland-test-portal'."
  (list (list :array
              (list :dict-entry "org.gnome.desktop.interface"
                    (cons :array
                          (mapcar (lambda (setting)
                                    (list :dict-entry (car setting)
                                          (list :variant (cdr setting))))
                                  wayland-test-portal))))))

(defun wayland-test-portal-set (key value)
  "Change KEY to VALUE and signal the change, as the portal does."
  (setf (alist-get key wayland-test-portal nil nil #'equal) value)
  (dbus-send-signal :session nil "/org/freedesktop/portal/desktop"
                    "org.freedesktop.portal.Settings" "SettingChanged"
                    "org.gnome.desktop.interface" key (list :variant value)))

(defun wayland-test-font-size (&optional frame)
  "The pixel size of FRAME's default font."
  (aref (font-info (face-font 'default frame) frame) 2))

(defun wayland-test-until (predicate)
  "Handle events until PREDICATE holds, for at most 5 seconds.
Read them, since D-Bus events wait behind any pending input, which
`sit-for' would leave unread."
  (let ((end (+ (float-time) 5)))
    (while (and (not (funcall predicate)) (< (float-time) end))
      (read-event nil nil 0.05))))

(defun wayland-test-until-size (size)
  "Handle events until the default font is SIZE pixels."
  (wayland-test-until (lambda () (= (wayland-test-font-size) size)))
  (should (equal (wayland-test-font-size) size)))

(ert-deftest wayland-test-desktop-settings ()
  ;; The run started without a portal: no system font, 96 dpi.
  (should-not (font-get-system-font))
  (should (= (wayland-test-font-size) 13))
  ;; Serve the portal on the test's own bus and read it again.
  (dbus-register-service :session "org.freedesktop.portal.Desktop")
  (dbus-register-method :session "org.freedesktop.portal.Desktop"
                        "/org/freedesktop/portal/desktop"
                        "org.freedesktop.portal.Settings" "ReadAll"
                        #'wayland-test-portal-read-all)
  (wayland--watch-desktop-settings)
  (should (equal (font-get-system-font) "DejaVu Sans Mono 12"))
  (should (equal (font-get-system-normal-font) "DejaVu Sans 11"))
  ;; The text scaling factor resizes fonts as it does in GTK.  The
  ;; frame has no font of its own, so reconsidering its fonts also takes
  ;; the system font: 12 points at 96 dpi times 1.5.
  (wayland-test-portal-set "text-scaling-factor" 1.5)
  (wayland-test-until-size 24)
  ;; The system font applies dynamically only with
  ;; `font-use-system-font'.
  (wayland-test-portal-set "monospace-font-name" "DejaVu Sans Mono 10")
  (wayland-test-until (lambda () (equal (font-get-system-font)
                                        "DejaVu Sans Mono 10")))
  (should (equal (font-get-system-font) "DejaVu Sans Mono 10"))
  (should (= (wayland-test-font-size) 24))
  (setq font-use-system-font t)
  (wayland-test-portal-set "monospace-font-name" "DejaVu Sans Mono 14")
  (wayland-test-until-size 28)
  ;; A new frame without a font of its own takes the system font.
  (let* ((frame (make-frame '((name . "Wayland desktop"))))
         (info (font-info (frame-parameter frame 'font) frame)))
    (should (string-match-p "DejaVu Sans Mono" (aref info 1)))
    (should (= (aref info 2) 28))
    (delete-frame frame))
  (wayland-test-send "ready")
  (wayland-test-next "done"))

(ert-deftest wayland-test-minimal-protocols ()
  (wayland-test-text-frame)
  (redisplay t)
  (dolist (binding '(("wl_compositor" . 4) ("wl_seat" . 4)
                     ("xdg_wm_base" . 1) ("wl_data_device_manager" . 1)))
    (should (member binding (wayland-test-metrics :globals))))
  (should (= (display-pixel-width) 1920))
  (should (= (display-pixel-height) 1080))
  (should (equal (alist-get 'geometry (frame-monitor-attributes))
                 '(0 0 1920 1080)))
  (gui-set-selection 'PRIMARY "local")
  (should (equal (gui-get-selection 'PRIMARY 'UTF8_STRING) "local"))
  (raise-frame)
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-next "key")
  (should (eq (read-event nil nil 2) ?a))
  (wayland-test-send "key")
  (wayland-test-next "done"))

;;; Soak.

(defun wayland-test-soak-state ()
  "The resources a soak checkpoint compares: frames, buffers, pools."
  (let ((metrics (wayland--metrics wayland-test-terminal)))
    (list :frames (length (frame-list))
          :buffers (length (buffer-list))
          :processes (length (process-list))
          :pool_bytes (plist-get metrics :pool-bytes)
          :retired_bytes (plist-get metrics :retired-bytes)
          :charged_pool_bytes (plist-get metrics :charged-pool-bytes)
          :discards (plist-get metrics :discards)
          :gcs gcs-done)))

(defun wayland-test-soak ()
  "Serve the soak runner's commands until it says done.
Input reaches the frame through the command loop meanwhile."
  (wayland-test-text-frame)
  (buffer-disable-undo)
  (let ((child nil)
        (buffer (current-buffer)))
    (set-process-filter
     wayland-test-connection
     (lambda (_process text)
       (setq wayland-test-input (concat wayland-test-input text))
       (while (string-match "\n" wayland-test-input)
         (let ((command (substring wayland-test-input 0 (match-beginning 0))))
           (setq wayland-test-input (substring wayland-test-input (match-end 0)))
           (pcase command
             ("child-show"
              (unless (frame-live-p child)
                (setq child (make-frame
                             `((parent-frame . ,wayland-test-frame)
                               (left . 40) (top . 40) (width . 30) (height . 8)
                               (minibuffer . nil) (name . "Wayland soak child")))))
              (wayland-test-send "child-show"))
             ("child-hide"
              (when (frame-live-p child)
                (delete-frame child))
              (setq child nil)
              (wayland-test-send "child-hide"))
             ((rx bos "copy " (let value (+ nonl)) eos)
              (gui-set-selection 'CLIPBOARD value)
              (wayland-test-send "copy" (if (wayland--selection-owner-p nil)
                                            t :false)))
             ("paste"
              ;; Never wait for a transfer inside a process filter.
              (run-at-time 0 nil
                           (lambda ()
                             (wayland-test-send
                              "paste" (or (gui-get-selection 'CLIPBOARD) "")))))
             ("checkpoint"
              (when (frame-live-p child)
                (delete-frame child))
              (setq child nil)
              (with-current-buffer buffer
                (erase-buffer))
              (let ((live 0))
                ;; Live Lisp bytes: each type's size times its count.
                (dolist (type (garbage-collect))
                  (when (and (consp (cdr type)) (numberp (nth 1 type))
                             (numberp (nth 2 type)))
                    (setq live (+ live (* (nth 1 type) (nth 2 type))))))
                (wayland-test-send "checkpoint"
                                   (append (wayland-test-soak-state)
                                           (list :lisp_bytes live)))))
             ("done" (kill-emacs 0)))))))
    (redisplay t)
    (wayland-test-send "ready" (wayland-test-capture))))

;;; m4.el ends here
