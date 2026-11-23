;;; m2.el --- Wayland input probes run by run.py -*- lexical-binding: t -*-

;; This file is part of GNU Emacs, as modified by the native Wayland
;; backend fork.
;; It is free software under the GNU General Public License, version 3
;; or later.

;;; Code:

(require 'ert)

(defun wayland-test-describe-events (count)
  "Read COUNT events and describe them, \"nil\" for a timeout."
  (let (events)
    (dotimes (_ count)
      (push (read-event nil nil 2) events))
    (mapconcat (lambda (event)
                 (if event (single-key-description event) "nil"))
               (nreverse events) " ")))

(defun wayland-test-frame-xy (position)
  "The frame pixel coordinates of POSITION.
`posn-x-y' is relative to the window's text area, after its fringe."
  (let ((xy (posn-x-y position))
        (window (posn-window position)))
    (if (and (windowp window) (null (posn-area position)))
        (let ((edges (window-inside-pixel-edges window)))
          (cons (+ (car xy) (nth 0 edges)) (+ (cdr xy) (nth 1 edges))))
      xy)))

(defun wayland-test-input-loop ()
  "Answer the runner's input commands until it says done."
  (let (command)
    (while (not (equal (setq command (wayland-test-read-command)) "done"))
      (pcase command
        ((rx bos "read " (let count (+ digit)) eos)
         (wayland-test-send "keys" (wayland-test-describe-events
                                    (string-to-number count))))
        ("drain"
         ;; Every event until half a second passes without one.
         (let (events event)
           (while (setq event (read-event nil nil 0.5))
             (push event events))
           (wayland-test-send "drained"
                              (mapconcat #'single-key-description
                                         (nreverse events) " "))))
        ("absolute"
         ;; Handle the motion the runner sent first.
         (input-pending-p)
         (let ((position (mouse-absolute-pixel-position)))
           (wayland-test-send "absolute" (vector (car position) (cdr position)))))
        ("queue-state"
         (wayland-test-send "queue-state"
                            (vector (wayland-test-metrics :raw-keys)
                                    (wayland-test-metrics :dropped-events))))
        ("hold"
         ;; Keep Lisp away from the display socket until the runner
         ;; says so.  Everything it sends meanwhile is then read and
         ;; dispatched at once.
         (wayland--debug-sigio nil wayland-test-terminal)
         (wayland-test-send "hold")
         (let ((resume (concat (getenv "EMACS_WAYLAND_TEST_RESUME") ".hold")))
           (while (not (file-exists-p resume))))
         (wayland--debug-sigio t wayland-test-terminal))
        ("queue-selection"
         (should (eq (read-event nil nil 2) ?a))
         (gui-set-selection 'CLIPBOARD "queue clipboard")
         (wayland-test-send "queue-selection"))
        ((or "busy" "wait")
         ;; C-g must interrupt busy Lisp, and a process wait that does
         ;; not read the keyboard.
         (wayland-test-send command (vector (if (car (current-input-mode)) "t" "nil")))
         (let* ((start (float-time))
                (before (wayland-test-metrics))
                ;; The fixture runs from a timer, which binds
                ;; `inhibit-quit' to t; a command runs with it nil.
                (result (condition-case nil
                            (let ((inhibit-quit nil))
                              (if (equal command "busy")
                                  (while (< (- (float-time) start) 10)
                                    (ignore))
                                (accept-process-output nil 10))
                              "timeout")
                          (quit "quit"))))
           (let ((after (wayland-test-metrics)))
             (wayland-test-send
              "quit" (vector result (- (float-time) start)
                             (- (plist-get after :hook-calls)
                                (plist-get before :hook-calls))
                             (- (plist-get after :raw-keys)
                                (plist-get before :raw-keys)))))))
        ((rx bos "clicks " (let count (+ digit)) eos)
         ;; Mouse events as [TYPE START-X START-Y END-X END-Y], and any
         ;; other event as [DESCRIPTION 0 0 0 0].
         (let (events)
           (dotimes (_ (string-to-number count))
             (let ((event (read-event nil nil 2)))
               (push (if (and (consp event) (posnp (event-start event)))
                         (let ((start (wayland-test-frame-xy
                                       (event-start event)))
                               (end (wayland-test-frame-xy (event-end event))))
                           (vector (symbol-name (car event))
                                   (car start) (cdr start)
                                   (car end) (cdr end)))
                       (vector (if event (format "%S" event) "nil") 0 0 0 0))
                     events)))
           (wayland-test-send "clicks" (vconcat (nreverse events)))))
        ((rx bos "wheel " (let count (+ digit)) eos)
         ;; Wheel events as [TYPE LINES DX DY CLASS]; LINES is 0 without
         ;; a line count, the deltas 0.0 without pixel deltas, and CLASS
         ;; the class of the device the event came from.
         (let (events)
           (dotimes (_ (string-to-number count))
             (let ((event (read-event nil nil 2)))
               (push (if (consp event)
                         (let ((lines (nth 3 event))
                               (delta (nth 4 event)))
                           (vector (symbol-name (car event))
                                   (if (numberp lines) lines 0)
                                   (float (or (car-safe delta) 0))
                                   (float (or (cdr-safe delta) 0))
                                   (symbol-name
                                    (device-class last-event-frame
                                                  last-event-device))))
                       (vector (if event (format "%S" event) "nil") 0 0.0 0.0
                               "nil"))
                     events)))
           (wayland-test-send "wheel" (vconcat (nreverse events)))))
        ((rx bos "coalesce " (let value (or "t" "nil")) eos)
         (setq mwheel-coalesce-scroll-events (equal value "t"))
         (wayland-test-send "coalesce"))
        ("preedit-state"
         ;; Preedit events are special events, handled while reading.
         (read-event nil nil 0.3)
         (wayland-test-send "preedit"
                            (or (and wayland-preedit-overlay
                                     (substring-no-properties
                                      (overlay-get wayland-preedit-overlay
                                                   'before-string)))
                                "")))
        ("cursor-rect"
         ;; Point's glyph in frame pixels: [X Y WIDTH HEIGHT].
         (redisplay t)
         (let* ((posn (posn-at-point))
                (xy (wayland-test-frame-xy posn))
                (size (posn-object-width-height posn)))
           (wayland-test-send "cursor-rect"
                              (vector (car xy) (cdr xy)
                                      (car size) (cdr size)))))
        ((rx bos "move " (let count (opt "-") (+ digit)) eos)
         (forward-char (string-to-number count))
         (redisplay t)
         (wayland-test-send "moved"))
        ("second-frame"
         ;; Each frame shows its own buffer, for "type".  The first
         ;; starts with a label, so that the frames' cursors differ.
         (switch-to-buffer (get-buffer-create "*im-first*"))
         (insert "first: ")
         (let ((frame (make-frame '((name . "Wayland second")))))
           (with-selected-frame frame
             (switch-to-buffer (get-buffer-create "*im-second*"))
             (redisplay t))
           (wayland-test-send "second" (vconcat (wayland--dump-frame nil frame)))))
        ("delete-second"
         ;; Collect the deleted frame, so that a pointer kept to it
         ;; would dangle.
         (delete-frame (seq-find (lambda (frame)
                                   (equal (frame-parameter frame 'name)
                                          "Wayland second"))
                                 (frame-list)))
         (garbage-collect)
         (wayland-test-send "deleted" (vector (length (frame-list)))))
        ("type"
         ;; Apply the pending events as the command loop would: a
         ;; switch-frame selects its frame, and a character inserts into
         ;; the selected window's buffer.  Answer each frame's text and
         ;; the other events, for at most 5 seconds.
         (let ((end (+ (float-time) 5)) event others)
           (while (and (< (float-time) end)
                       (setq event (read-event nil nil 0.5)))
             (cond ((eq (car-safe event) 'switch-frame)
                    (select-frame (cadr event)))
                   ((characterp event)
                    (with-current-buffer (window-buffer (selected-window))
                      (insert event)))
                   (t (push (format "%S" (car-safe event)) others))))
           (wayland-test-send "typed"
                              (vector (with-current-buffer "*im-first*"
                                        (buffer-string))
                                      (with-current-buffer "*im-second*"
                                        (buffer-string))
                                      (vconcat (nreverse others))))))
        ((rx bos "use-im " (let value (or "t" "nil")) eos)
         (setq wayland-use-input-method (equal value "t"))
         (wayland-test-send "use-im"))
        ("geometry"
         ;; The mode line's buffer name, the tool bar's height, character
         ;; sizes and the selected window's height, which sets the pixels
         ;; per scroll unit.
         (let* ((edges (window-pixel-edges))
                (mode-line-y (- (nth 3 edges)
                                (/ (window-mode-line-height) 2))))
           (wayland-test-send "geometry"
                              (vector mode-line-y
                                      (* 10 (frame-char-width))
                                      (tool-bar-height nil t)
                                      (frame-char-width)
                                      (frame-char-height)
                                      (window-pixel-height)))))
        ("tool-bar"
         (tool-bar-mode 1)
         (redisplay t)
         (wayland-test-send "tool-bar" (vector (tool-bar-height nil t))))
        ("focus-state"
         ;; Focus events are special events, handled while reading.
         (read-event nil nil 0.2)
         (wayland-test-send "focus" (if (frame-focus-state wayland-test-frame)
                                        "t" "nil")))
        ("mouse-state"
         ;; Handle the seat's capability change first.
         (read-event nil nil 0.2)
         (wayland-test-send "mouse" (if (display-mouse-p wayland-test-frame)
                                        "t" "nil")))
        (_ (ert-fail command))))))

(defun wayland-test-input ()
  (wayland-test-text-frame)
  (redisplay t)
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-input-loop))

(ert-deftest wayland-test-keyboard ()
  (wayland-test-input))

(ert-deftest wayland-test-key-repeat ()
  (wayland-test-input))

(ert-deftest wayland-test-seat-changes ()
  (wayland-test-input))

(ert-deftest wayland-test-input-queue ()
  (wayland-test-input))

(ert-deftest wayland-test-quit ()
  (wayland-test-input))

(defun wayland-test-pointer-frame ()
  ;; Text under the pointer, and a mode line whose buffer name has a
  ;; mouse face.
  (wayland-test-text-frame)
  (setq-local mode-line-format (default-value 'mode-line-format))
  (dotimes (row 20)
    (insert (format "Pointer test line %02d with some text\n" row)))
  (goto-char (point-min))
  (redisplay t)
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-input-loop))

(ert-deftest wayland-test-pointer ()
  (wayland-test-pointer-frame))

(ert-deftest wayland-test-pointer-theme ()
  (wayland-test-pointer-frame))

(ert-deftest wayland-test-scroll ()
  (wayland-test-pointer-frame))

(ert-deftest wayland-test-scroll-v5 ()
  (wayland-test-pointer-frame))

(ert-deftest wayland-test-scroll-v4 ()
  (wayland-test-pointer-frame))

(ert-deftest wayland-test-fcitx5 ()
  (wayland-test-text-frame)
  (setq-local cursor-type 'box)
  (redisplay t)
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-input-loop))

(ert-deftest wayland-test-input-method ()
  ;; A visible cursor, whose rectangle the input method gets.
  (wayland-test-text-frame)
  (setq-local cursor-type 'box)
  (dotimes (row 10)
    (insert (format "Input method line %02d\n" row)))
  (goto-char (point-min))
  (redisplay t)
  (wayland-test-send "ready" (wayland-test-capture))
  (wayland-test-input-loop))

(defun wayland-test-describe-event (event)
  "Describe EVENT for comparison between backends.
Mouse events also name the column and row of their start and, for a
drag, of their end."
  (let ((description (key-description (vector event))))
    (if (not (mouse-event-p event))
        description
      (let ((start (posn-col-row (event-start event)))
            (end (posn-col-row (event-end event))))
        (concat description
                (format "@%d,%d" (car start) (cdr start))
                (and (not (equal start end))
                     (format "->%d,%d" (car end) (cdr end))))))))

(defun wayland-test-read-events (phase)
  "Report the events that arrive until input is quiet as PHASE."
  (let (events event)
    (while (setq event (read-event nil nil 0.75))
      (unless (memq (car-safe event) '(switch-frame focus-in focus-out))
        (push (wayland-test-describe-event event) events)))
    (wayland-test-send phase (vconcat (nreverse events)))))

(ert-deftest wayland-test-input-parity ()
  (wayland-test-text-frame)
  (scroll-bar-mode -1)
  (dotimes (_ 40) (insert "A row for the input parity test\n"))
  (goto-char (point-min))
  (wayland-test-wait
   (lambda ()
     (redisplay t)
     (and (= (frame-pixel-width) 1024) (= (frame-pixel-height) 768))))
  (wayland-test-send "ready" (list :backend (symbol-name window-system)
                                   :char-width (frame-char-width)
                                   :char-height (frame-char-height)
                                   :window-height (window-pixel-height)))
  (wayland-test-next "keys")
  (wayland-test-read-events "keys")
  (wayland-test-next "pointer")
  (wayland-test-read-events "pointer")
  ;; Report the pixel deltas that `pixel-scroll-precision-mode' reads.
  (setq mwheel-coalesce-scroll-events nil)
  (wayland-test-next "precision")
  (let (deltas event)
    (while (setq event (read-event nil nil 0.75))
      (when (memq (event-basic-type event) '(wheel-up wheel-down))
        (push (cdr (nth 4 event)) deltas)))
    (wayland-test-send "precision" (vconcat (nreverse deltas))))
  (wayland-test-next "done"))

(defun wayland-test-benchmark-buffer ()
  "Show a fontified copy of the benchmark source in the test frame.
Wait until the frame has the fullscreen size the runner expects."
  (wayland-test-text-frame)
  (set-frame-font "DejaVu Sans Mono-14" nil (list wayland-test-frame))
  (erase-buffer)
  (insert-file-contents (getenv "EMACS_WAYLAND_TEST_SOURCE"))
  (c-mode)
  (setq-local truncate-lines t cursor-type nil mode-line-format nil)
  (font-lock-mode 1)
  (display-line-numbers-mode 1)
  (font-lock-ensure (point-min) (min (point-max) (+ (point-min) 4096)))
  (goto-char (point-min))
  (let ((size (mapcar #'string-to-number
                      (split-string (getenv "EMACS_WAYLAND_TEST_SIZE")))))
    (while (not (equal size (list (frame-pixel-width) (frame-pixel-height))))
      (redisplay t)
      (sit-for 0.02))))

(defun wayland-test-build-details ()
  (list :backend (symbol-name window-system)
        :width (frame-pixel-width) :height (frame-pixel-height)
        :version emacs-version
        :commit emacs-repository-version
        :directory invocation-directory
        :source (expand-file-name source-directory)
        :options system-configuration-options
        :features system-configuration-features
        :native (if (featurep 'native-compile) t :false)))

(defun wayland-test-perf-state ()
  "Return native Wayland's commit and pool counters, or nil."
  (and (eq (window-system wayland-test-frame) 'wayland)
       (let ((metrics (wayland--metrics wayland-test-terminal)))
         (list :commits (cadr (wayland--dump-frame nil wayland-test-frame))
               :pool_bytes (plist-get metrics :pool-bytes)
               :retired_bytes (plist-get metrics :retired-bytes)
               :presentation (when (getenv "EMACS_WAYLAND_TEST_PRESENTATION")
                               (wayland--presentation-feedback
                                nil wayland-test-terminal))))))

(defun wayland-test-scroll-benchmark ()
  "Scroll the benchmark buffer by the runner's touchpad strokes.
On \"scrolled\", report how far the window scrolled, in frame pixels,
once input has been quiet for half a second."
  (wayland-test-benchmark-buffer)
  (pixel-scroll-precision-mode 1)
  ;; Xwayland names the virtual touchpad a mouse.  Use touchpad
  ;; precision scrolling on it too, without the mouse animation.
  (setq pixel-scroll-precision-interpolate-mice nil)
  (redisplay t)
  (let (images)
    (set-process-filter
     wayland-test-connection
     (lambda (_process text)
       (setq wayland-test-input (concat wayland-test-input text))
       (while (string-match "\n" wayland-test-input)
         (pcase (wayland-test-read-command)
           ("perf-state"
            (wayland-test-send "perf-state" (wayland-test-perf-state)))
           ("trace-start"
            (wayland--trace wayland-test-terminal t)
            (wayland-test-send "trace-start"))
           ("trace"
            (wayland-test-send "trace" (wayland--trace wayland-test-terminal)))
           ("image-scroll-setup"
            (wayland-test-perf-image-buffer)
            (setq images t)
            (redisplay t)
            (wayland-test-send "image-scroll-setup"))
           ("scroll-reset"
            (goto-char (point-min))
            (set-window-start nil (point-min))
            (set-window-vscroll nil 0)
            (redisplay t)
            (wayland-test-send "scroll-reset"))
           ("scrolled"
            (run-with-idle-timer
             0.5 nil
             (lambda ()
               (let* ((window (frame-selected-window wayland-test-frame))
                      (start (window-start window))
                      (line (if images
                                (car (window-line-height 0 window))
                              (default-line-height))))
                 (wayland-test-send
                  "scrolled"
                  (list :pixels
                        (+ (if images
                               (cdr (window-text-pixel-size
                                     window (point-min) start nil nil nil t))
                             (* (1- (line-number-at-pos start)) line))
                           (window-vscroll window t))
                        :line-height line))))))
           ("done" (kill-emacs 0)))))))
  (wayland-test-send
   "ready"
   (append (wayland-test-build-details)
           (list :window-height (window-pixel-height)
                 :settings
                 (mapcar (lambda (value)
                           (cond ((keywordp value) value) (value t) (t :false)))
                         (list :pixel_scroll_precision_mode
                               pixel-scroll-precision-mode
                               :coalesce mwheel-coalesce-scroll-events
                               :momentum pixel-scroll-precision-use-momentum
                               :interpolate_mice
                               pixel-scroll-precision-interpolate-mice
                               :interpolate_page
                               pixel-scroll-precision-interpolate-page))))))

(defun wayland-test-observer (&optional benchmark)
  "Leave Emacs in its command loop for the screencopy observer.
BENCHMARK prepares a fontified source buffer and reports its capture region
and build details.  Otherwise use an empty first line for the M2 probe."
  (if (not benchmark)
      (wayland-test-text-frame)
    (wayland-test-benchmark-buffer)
    ;; Start on the blank line after the opening comment line.
    (forward-line 1))
  (unless benchmark
    (erase-buffer)
    (insert "\n")
    (goto-char (point-min)))
  (redisplay t)
  (let ((start (point))
        (buffer (current-buffer))
        (edges (window-inside-pixel-edges)))
    (set-process-filter
     wayland-test-connection
     (lambda (_process text)
       (setq wayland-test-input (concat wayland-test-input text))
       (while (string-match "\n" wayland-test-input)
         (let ((command (substring wayland-test-input 0 (match-beginning 0))))
           (setq wayland-test-input
                 (substring wayland-test-input (match-end 0)))
           (pcase command
             ("perf-state"
              (wayland-test-send "perf-state" (wayland-test-perf-state)))
             ("trace-start"
              (wayland--trace wayland-test-terminal t)
              (wayland-test-send "trace-start"))
             ("trace"
              (wayland-test-send "trace" (wayland--trace wayland-test-terminal)))
             ("blink"
              ;; Blink without the usual limit, for the idle margin check.
              (with-current-buffer buffer
                (setq-local cursor-type t))
              (setq blink-cursor-blinks 0)
              (blink-cursor-mode 1)
              (wayland-test-send "blink" (frame-focus-state wayland-test-frame)))
             ("done"
              (when benchmark
                (with-current-buffer buffer
                  (save-excursion
                    (goto-char start)
                    (wayland-test-send
                     "typed" (buffer-substring-no-properties
                              start (line-end-position))))))
              (kill-emacs 0)))))))
    (wayland-test-send
     "ready"
     (if benchmark
         (let* ((pos (posn-at-point))
                (xy (posn-x-y pos))
                (left (+ (car edges) (car xy)))
                (top (+ (cadr edges) (cdr xy))))
           (append (wayland-test-build-details)
                   (list :bounds (vector left top (nth 2 edges)
                                         (+ top (frame-char-height))))))
       (vector (nth 0 edges) (nth 1 edges)
               (- (nth 2 edges) (nth 0 edges)) (frame-char-height))))))

;;; m2.el ends here
