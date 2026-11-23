;;; m1.el --- Wayland rendering probes run by run.py -*- lexical-binding: t -*-

;; This file is part of GNU Emacs, as modified by the native Wayland
;; backend fork.
;; It is free software under the GNU General Public License, version 3
;; or later.

;;; Code:

(require 'ert)

(defun wayland-test-paint (background)
  (modify-frame-parameters wayland-test-frame
                           `((background-color . ,background)))
  (force-window-update t)
  (redisplay t))

(defun wayland-test-capture ()
  (vconcat (wayland--dump-frame (getenv "EMACS_WAYLAND_TEST_DUMP")
                                wayland-test-frame)))

(defun wayland-test-render-text (text)
  (erase-buffer)
  (insert text)
  (goto-char (point-min))
  (force-window-update t)
  (redisplay t))

(defun wayland-test-text-frame ()
  (tool-bar-mode -1)
  (menu-bar-mode -1)
  (tab-bar-mode -1)
  (blink-cursor-mode -1)
  (switch-to-buffer (get-buffer-create "*Wayland rendering test*"))
  (setq-local cursor-type nil mode-line-format nil)
  (set-frame-font "Monospace-14" nil (list wayland-test-frame)))

(ert-deftest wayland-test-glyph-pixels ()
  (wayland-test-text-frame)
  (should (= (string-pixel-width "WWWWWWWW")
             (string-pixel-width "IIIIIIII")))
  ;; Cairo returns no glyph for a NUL, which encode_char must not read.
  (let ((font (font-at 0 nil "a")))
    (should (aref (font-get-glyphs font 0 1 "a") 0))
    (should-not (aref (font-get-glyphs font 0 1 "\0") 0)))
  (redisplay t)
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-next "first")
  (wayland-test-render-text "WWWWWWWW")
  (wayland-test-next "second")
  (wayland-test-render-text "IIIIIIII")
  (wayland-test-next "done")
  (wayland--dump-frame nil wayland-test-frame))

(ert-deftest wayland-test-interrupted-redraw ()
  (wayland-test-text-frame)
  (wayland-test-render-text "M1 BEFORE\nM1 BEFORE\nM1 BEFORE")
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-next "interrupt")
  (should
   (catch 'wayland--test-interrupted-draw
     (wayland--debug-interrupt-after-draw 1 wayland-test-terminal)
     (wayland-test-render-text "M1 RECOVERED\nM1 RECOVERED\nM1 RECOVERED")
     nil))
  (wayland-test-send "interrupted")
  (wayland-test-next "redraw")
  (force-window-update t)
  (redisplay t)
  (wayland-test-next "done")
  (wayland--dump-frame nil wayland-test-frame))

(ert-deftest wayland-test-pixel-baseline ()
  (tool-bar-mode -1)
  (blink-cursor-mode -1)
  (redisplay t)
  (let* ((directory (getenv "EMACS_WAYLAND_TEST_DUMP"))
         (capture (wayland--dump-frame directory wayland-test-frame)))
    (wayland-test-send "ready" (vconcat capture))
    (wayland-test-next "first")
    (wayland-test-paint "#146c94")
    (wayland-test-next "second")
    (wayland-test-paint "#b24635")
    (wayland-test-next "done")
    (wayland--dump-frame nil wayland-test-frame)))

(ert-deftest wayland-test-commit-loss ()
  (let ((survivor (wayland-test-survivor)))
    (wayland-test-send "ready")
    (wayland-test-next "draw")
    (wayland-test-paint "#146c94")
    (while (frame-live-p wayland-test-frame)
      (read-event nil nil 0.05))
    (wayland-test-after-loss survivor)))

(defun wayland-test-m1-metrics ()
  (let ((metrics (wayland-test-metrics)))
    (vconcat
     (mapcar (lambda (key) (plist-get metrics key))
             '(:pool-bytes :retired-bytes :charged-pool-bytes :pool-budget
               :repair-bytes :full-repairs :last-buffer-age :scroll-bytes
               :ack-only-commits :last-release-ns :discards)))))

(defun wayland-test-m1-frame ()
  (wayland-test-text-frame)
  (erase-buffer)
  (dotimes (row 90)
    (insert (format "W row %02d: Wayland rendering\n" row)))
  (goto-char (point-min))
  (redisplay t))

(defun wayland-test-subpixel-frame ()
  "Text whose changed runs of glyphs start and end beside unchanged ink.
`swap' turns each W into I and back: a W's filter pixels reach the
neighboring cells, and an I's do not.  The header line changes its
word and face, as a status display does."
  (wayland-test-text-frame)
  (erase-buffer)
  (setq-local header-line-format
              '(:eval (if (bound-and-true-p wayland-test-swapped)
                          (propertize "THNK" 'face 'warning)
                        (propertize "IDLE" 'face 'success))))
  (dotimes (row 12)
    (insert (if (zerop (% row 2)) "" " ")
            "W" (propertize "xW" 'face 'bold) "|W"
            (propertize " W:" 'face 'italic) "W\n"))
  (goto-char (point-min))
  (redisplay t))

(defun wayland-test-m1-control (&optional setup)
  (funcall (or setup #'wayland-test-m1-frame))
  (wayland-test-send "ready" (wayland-test-capture))
  (let (command frames)
    (while (not (equal (setq command (wayland-test-read-command)) "done"))
      (pcase command
        ("resize" (force-window-update t))
        ("redraw" (redraw-frame))
        ("add-frames"
         (dotimes (index 2)
           (let ((frame (make-frame `((name . ,(format "M1 frame %d" index))))))
             (push frame frames))))
        ("fail-pool" (wayland--debug-fail-pool t))
        ("restore" (wayland--debug-fail-pool nil))
        ("hide" (make-frame-invisible wayland-test-frame t))
        ("show" (make-frame-visible wayland-test-frame))
        ("scroll" (scroll-up-line 1))
        ("swap"
         (setq-local wayland-test-swapped
                     (not (bound-and-true-p wayland-test-swapped)))
         (if wayland-test-swapped
             (subst-char-in-region (point-min) (point-max) ?W ?I)
           (subst-char-in-region (point-min) (point-max) ?I ?W))
         (force-mode-line-update))
        ("state" nil)
        ("idle"
         ;; Signal readiness from inside a single input wait, after
         ;; its initial redisplay has finished.  A key ends the wait.
         (let ((timer (run-at-time 0.1 nil #'wayland-test-send "idle")))
           (unwind-protect
               (read-event nil nil 15)
             (cancel-timer timer))))
        ((rx bos "font " (let height (+ digit)) eos)
         (set-face-attribute 'default wayland-test-frame
                             :height (string-to-number height)))
        ((rx bos "frame-size " (let width (+ digit)) " " (let height (+ digit))
             eos)
         (set-frame-size wayland-test-frame (string-to-number width)
                         (string-to-number height) t))
        ("geometry"
         (redisplay t)
         (wayland-test-send
          "geometry"
          (vector (frame-pixel-width) (frame-pixel-height)
                  (symbol-name (or (frame-parameter nil 'fullscreen) 'none)))))
        ((or "title" "iconify")
         ;; Hold Lisp here so a later wait or redisplay cannot send the
         ;; request on behalf of the frame setter.
         (wayland--debug-sigio nil wayland-test-terminal)
         (if (equal command "title")
             (modify-frame-parameters nil '((title . "Wayland flush test")))
           (iconify-frame)
           (should (frame-visible-p wayland-test-frame)))
         (wayland-test-send command)
         (let ((resume (concat (getenv "EMACS_WAYLAND_TEST_RESUME")
                               "." command)))
           (while (not (file-exists-p resume))))
         (wayland--debug-sigio t wayland-test-terminal))
        ((rx bos "edit " (let row (+ digit)) eos)
         (save-excursion
           (goto-char (point-min))
           (forward-line (string-to-number row))
           (subst-char-in-region (point) (1+ (point))
                                 (char-after) (if (eq (char-after) ?W) ?M ?W))))
        (_ (ert-fail command)))
      (redisplay t)
      (wayland-test-send "state" (wayland-test-m1-metrics)))))

(ert-deftest wayland-test-pool-lifetime ()
  (wayland-test-m1-control))

(ert-deftest wayland-test-damage-repair ()
  (wayland-test-m1-control))

(ert-deftest wayland-test-damage-repair-rgb ()
  (wayland-test-m1-control))

(ert-deftest wayland-test-subpixel-redraw ()
  (wayland-test-m1-control #'wayland-test-subpixel-frame))

(ert-deftest wayland-test-pool-budget ()
  (wayland-test-m1-control))

(ert-deftest wayland-test-pool-budget-recovery ()
  (wayland-test-m1-control))

(ert-deftest wayland-test-frame-states ()
  (wayland-test-m1-control))

(ert-deftest wayland-test-mid-draw ()
  ;; SIGIO would dispatch before the probe under test.
  (wayland--debug-sigio nil wayland-test-terminal)
  (wayland-test-m1-frame)
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-next "draw")
  (let (armed)
    (let ((pre-redisplay-function
           (lambda (_frames)
             (unless armed
               (setq armed t)
               (wayland-test-send "drawing-next")
               (while (not (file-exists-p (getenv "EMACS_WAYLAND_TEST_RESUME"))))
               (wayland--debug-dispatch-after-draw 1 wayland-test-terminal)))))
      (wayland-test-render-text "M1 release and configure during glyph drawing\n"))
    (should armed))
  (wayland-test-send "drew")
  (wayland-test-next "redraw")
  (force-window-update t)
  (redisplay t)
  (wayland-test-next "done"))

(ert-deftest wayland-test-mid-redisplay-write ()
  ;; SIGIO would dispatch before the probe under test.
  (wayland--debug-sigio nil wayland-test-terminal)
  (wayland-test-m1-frame)
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-next "draw")
  (let ((resumes (wayland-test-metrics :write-resumes)) armed)
    (let ((pre-redisplay-function
           (lambda (_frames)
             (unless armed
               (setq armed t)
               (should (eq (wayland--debug-request-burst
                            50000 nil wayland-test-terminal) 'pending))
               (wayland-test-send "pending")
               (wayland-test-wait
                (lambda () (> (wayland-test-metrics :write-resumes) resumes)))
               (wayland-test-send "resumed")
               (wayland-test-next "leave")))))
      (wayland-test-render-text "M1 writable callback during redisplay\n"))
    (should armed))
  (wayland-test-next "done"))

(defun wayland-test-scale-state (frame)
  (with-selected-frame frame
    (let ((info (font-info (font-at (window-point)))))
      (vector (frame-pixel-width) (frame-pixel-height)
              (frame-char-width) (frame-char-height) (aref info 2)
              (frame-width) (frame-height)
              (vconcat (alist-get 'geometry (frame-monitor-attributes frame)))
              (alist-get 'name (frame-monitor-attributes frame))))))

(defun wayland-test-scale-frames ()
  (wayland-test-m1-frame)
  (let* ((font (font-spec :family "Monospace" :size 14.0))
         (second (make-frame '((name . "Wayland second"))))
         (monitor-changes 0)
         (display-monitors-changed-functions
          (list (lambda (terminal)
                  (when (eq terminal wayland-test-terminal)
                    (cl-incf monitor-changes))))))
    (set-frame-font font nil (list wayland-test-frame))
    (with-selected-frame second
      (switch-to-buffer (get-buffer-create "*Wayland second*"))
      (erase-buffer)
      (insert "Second Wayland frame\n")
      (goto-char (point-min))
      (setq-local cursor-type nil mode-line-format nil)
      (set-frame-font font nil (list second)))
    (force-window-update t)
    (redisplay t)
    (wayland-test-send "ready" (wayland-test-capture))
    (wayland-test-send "second"
                       (vconcat (wayland--dump-frame
                                 (getenv "EMACS_WAYLAND_TEST_DUMP") second)))
    (let (command)
      (while (not (equal (setq command (wayland-test-read-command)) "done"))
        (should (member command '("state" "redraw")))
        (when (equal command "redraw")
          (force-window-update t))
        (redisplay t)
        (read-event nil nil 0.01)
        (wayland-test-send "state"
                           (vector (wayland-test-scale-state wayland-test-frame)
                                   (wayland-test-scale-state second)
                                   (vector (display-pixel-width)
                                           (display-pixel-height))
                                   monitor-changes))))))

(ert-deftest wayland-test-scale-fractional ()
  (wayland-test-scale-frames))
(ert-deftest wayland-test-scale-no-viewporter ()
  (wayland-test-scale-frames))
(ert-deftest wayland-test-scale-no-fractional ()
  (wayland-test-scale-frames))
(ert-deftest wayland-test-scale-outputs ()
  (wayland-test-scale-frames))

(defun wayland-test-setup-pixel-scene (font)
  (wayland-test-text-frame)
  (let (marker-start marker-end)
    (set-frame-font font nil (list wayland-test-frame))
    (modify-frame-parameters
     wayland-test-frame '((internal-border-width . 0)
                          (vertical-scroll-bars . nil)
                          (horizontal-scroll-bars . nil)
                          (background-color . "#ffffff")
                          (foreground-color . "#000000")))
    (set-window-fringes (selected-window) 12 12)
    (set-cursor-color "#ff0000")
    (set-face-attribute 'fringe wayland-test-frame
                        :foreground "#008000" :background "#ffffff")
    (define-fringe-bitmap 'wayland-test-bitmap [128 192 224 240 224 192 128])
    (erase-buffer)
    (let ((start (point)))
      (insert " Cursor on a glyph\n")
      (put-text-property start (1+ start) 'display
                         '(left-fringe wayland-test-bitmap fringe)))
    (insert (propertize "Clipped italic jf " 'face 'italic))
    (insert (make-string 140 ?W) "\n")
    (setq-local truncate-lines t)
    (insert "Image ")
    (insert-image (create-image "P3\n2 2\n255\n0 0 255 255 255 0\n255 255 0 0 0 255\n"
                                'pbm t :scale 1 :width 12 :height 12))
    (insert "\n")
    (setq marker-start (point))
    (insert "Frame marker\n")
    (setq marker-end (1- (point)))
    (goto-char (+ (point-min) 3))
    (cons marker-start marker-end)))

(ert-deftest wayland-test-pixel-parity ()
  (pcase-let ((`(,marker-start . ,marker-end)
               (wayland-test-setup-pixel-scene
                (font-spec :family "DejaVu Sans Mono" :size 18))))
    ;; Sway makes both windows fullscreen. Wait for the configure and
    ;; geometry before taking frame-relative coordinates for screencopy.
    (wayland-test-wait
     (lambda ()
       (redisplay t)
       (and (= (frame-pixel-width) 1024) (= (frame-pixel-height) 768))))
    (let ((info (font-info (font-at (point)))))
      (wayland-test-send "ready" (vector (symbol-name window-system)
                                        (aref info 12))))
    (dolist (phase '(("hbar" hbar "#007eb1" [0 126 177])
                     ("hollow" hollow "#a2006d" [162 0 109])
                     ("erased" nil "#c17100" [193 113 0])))
      (pcase-let ((`(,name ,cursor ,color ,rgb) phase))
        (wayland-test-next name)
        (setq-local cursor-type cursor cursor-in-non-selected-windows cursor)
        (put-text-property marker-start marker-end 'face
                           `(:background ,color :foreground ,color))
        (force-window-update t)
        (redisplay t)
        (wayland-test-send name (vector (vconcat (window-pixel-edges)) rgb))))
    (let* ((size 96)
           (canvas (list 'image :type 'canvas :id 'wayland-test-pixel-parity
                         :data-width size :data-height size
                         :data (make-vector (* size size) #xff147cb1)))
           (edges (vconcat (window-pixel-edges))))
      (wayland-test-next "canvas-initial")
      (erase-buffer)
      ;; `insert-image' copies the spec, but refresh needs the displayed
      ;; object itself.
      (insert (propertize "#" 'display canvas))
      (redisplay t)
      (wayland-test-send "canvas-initial" (vector edges [20 124 177]))
      (wayland-test-next "canvas-refresh")
      (plist-put (cdr canvas) :data (make-vector (* size size) #xffb14623))
      ;; Refresh draws directly into the backing image.  The following
      ;; redisplay must present it without changing the buffer text.
      (canvas-refresh canvas t)
      (redisplay)
      (wayland-test-send "canvas-refresh" (vector edges [177 70 35])))
    (wayland-test-next "done")))

(defconst wayland-test-image-directory
  (file-name-directory (or load-file-name buffer-file-name)))

(ert-deftest wayland-test-image-parity ()
  ;; Every image format the build decodes, drawn the same way by both
  ;; backends: the decoders and Cairo are shared, so the pixels match.
  (wayland-test-text-frame)
  (modify-frame-parameters
   wayland-test-frame '((internal-border-width . 0) (vertical-scroll-bars . nil)
                        (background-color . "#ffffff")
                        (foreground-color . "#000000")))
  (set-window-fringes (selected-window) 0 0)
  (setq-local cursor-type nil truncate-lines t)
  (erase-buffer)
  (let* ((images (expand-file-name "images/" wayland-test-image-directory))
         (etc (expand-file-name "images/" data-directory))
         (gif (create-image (expand-file-name "frames.gif" images) 'gif nil
                            :scale 2))
         (canvas (list 'image :type 'canvas :id 'wayland-test-image-parity
                       :data-width 32 :data-height 32
                       :data (make-vector 1024 #xff147cb1)))
         marker-start positions)
    (dolist (spec
             `(,(create-image (expand-file-name "splash.png" etc) 'png nil
                              :scale 0.25)
               ,(create-image (expand-file-name "splash.svg" etc) 'svg nil
                              :scale 0.25)
               ,(create-image "<svg xmlns='http://www.w3.org/2000/svg' \
width='48' height='48'><circle cx='24' cy='24' r='20' fill='#d02090' \
stroke='#000' stroke-width='3'/></svg>" 'svg t)
               ,(create-image (expand-file-name "gradient.jpg" images) 'jpeg
                              nil)
               ,(create-image (expand-file-name "gradient.webp" images) 'webp
                              nil)
               ,(create-image (concat "#define t_width 16\n#define t_height 16\n"
                                      "static char t_bits[] = {"
                                      (mapconcat (lambda (_) "0xaa")
                                                 (make-list 32 0) ",")
                                      "};")
                              'xbm t :foreground "#c04000" :background "#ffe080")
               ,gif))
      ;; Not `insert-image', which may copy the spec: the GIF's frame
      ;; is set on the displayed object.
      (push (point) positions)
      (insert (propertize "#" 'display spec) " "))
    (insert "\n")
    (dolist (spec
             (list (create-image (expand-file-name "gradient.tiff" images)
                                 'tiff nil)
                   (create-image
                    (concat "/* XPM */\nstatic char *test[] = {\n"
                            "\"2 2 2 1\",\n\"a c #6840a0\",\n\"b c #ffcc30\",\n"
                            "\"ab\",\n\"ba\"};")
                    'xpm t :width 24 :height 24)
                   (create-image "P1\n2 2\n0 1\n1 0\n"
                                 'pbm t :width 24 :height 24)
                   canvas))
      (push (point) positions)
      (insert (propertize "#" 'display spec) " "))
    (insert "\n")
    (setq marker-start (point))
    (insert "Frame marker\n")
    (goto-char (point-min))
    ;; Sway makes the frame fullscreen; the runner gives its size in
    ;; physical pixels when the output is scaled.
    (let ((size (mapcar #'string-to-number
                        (split-string (or (getenv "EMACS_WAYLAND_TEST_SIZE")
                                          "1024 768")))))
      (wayland-test-wait
       (lambda ()
         (redisplay t)
         (and (= (frame-pixel-width) (car size))
              (= (frame-pixel-height) (cadr size))))))
    ;; An image that fails to load shows its text, the same on both
    ;; backends.
    (dolist (position positions)
      (should (posn-image (posn-at-point position))))
    (wayland-test-send "ready" (vector (symbol-name window-system)))
    ;; Each phase changes the marker's color, so the runner knows when
    ;; the compositor shows the phase.
    (dolist (phase '(("images" 0 "#007eb1" [0 126 177])
                     ("gif-1" 1 "#a2006d" [162 0 109])
                     ("gif-2" 2 "#c17100" [193 113 0])))
      (pcase-let ((`(,name ,frame ,color ,rgb) phase))
        (wayland-test-next name)
        (image-show-frame gif frame t)
        (put-text-property marker-start (1- (point-max)) 'face
                           `(:background ,color :foreground ,color))
        (redisplay t)
        (wayland-test-send name (vector (vconcat (window-pixel-edges)) rgb
                                        (image-current-frame gif)))))
    (wayland-test-next "canvas-refresh")
    (plist-put (cdr canvas) :data (make-vector 1024 #xffb14623))
    (canvas-refresh canvas t)
    (redisplay)
    (wayland-test-send "canvas-refresh"
                       (vector (vconcat (window-pixel-edges)) [177 70 35] 2))
    (wayland-test-next "done")))

(ert-deftest wayland-test-scaled-pixels ()
  (pcase-let ((`(,start . ,end)
               (wayland-test-setup-pixel-scene
                (font-spec :family "DejaVu Sans Mono" :size 14.0)))
              (`(,width ,height)
               (mapcar #'string-to-number
                       (split-string (getenv "EMACS_WAYLAND_TEST_SIZE")))))
    (message nil)
    (wayland-test-wait
     (lambda ()
       (redisplay t)
       (and (= (frame-pixel-width) width) (= (frame-pixel-height) height))))
    (wayland-test-send "ready" (wayland-test-capture))
    (wayland-test-next "scene")
    ;; Initial density changes reconstruct frame-local faces.
    (set-cursor-color "#ff0000")
    (set-face-attribute 'fringe wayland-test-frame
                        :foreground "#008000" :background "#ffffff")
    (setq-local cursor-type 'hbar cursor-in-non-selected-windows 'hbar)
    (put-text-property start end 'face
                       '(:background "#007eb1" :foreground "#007eb1"))
    (force-window-update t)
    (redisplay t)
    (wayland-test-send "scene"
                       (vector (frame-pixel-width) (frame-pixel-height)
                               (frame-char-width)
                               ;; A tool bar icon, scaled like inline images.
                               (car (image-size
                                     (find-image
                                      '((:type xpm :file "images/new.xpm")))
                                     t))
                               ;; An explicit factor is in logical pixels.
                               (car (image-size
                                     (find-image
                                      '((:type xpm :file "images/new.xpm"
                                         :scale 1)))
                                     t))))
    (wayland-test-next "done")))

;;; m1.el ends here
