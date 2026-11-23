;;; wayland-win.el --- Native Wayland window system support -*- lexical-binding: t -*-

;; This file is part of GNU Emacs, as modified by the native Wayland
;; backend fork.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; This file is loaded by loadup.el in a native Wayland build.

;;; Code:

(eval-when-compile (require 'cl-lib))
(require 'term/common-win)
(require 'frame)
(require 'fontset)

(defvar x-display-name)
(declare-function x-handle-args "common-win" (args))
(declare-function x-open-connection "waylandfns.c"
                  (display &optional xrm-string must-succeed))
(declare-function x-display-list "waylandfns.c" ())

;; The socket name can be relative to XDG_RUNTIME_DIR or an absolute
;; path.  A native Wayland build has no other GUI backend.
(add-to-list 'display-format-alist '(".*" . wayland))

(cl-defmethod window-system-initialization (&context (window-system wayland)
                                            &optional display)
  (create-default-fontset)
  (condition-case err
      (create-fontset-from-fontset-spec standard-fontset-spec t)
    (error (display-warning 'initialization
                            (format "Creation of the standard fontset failed: %s"
                                    err)
                            :error)))
  (x-open-connection (or display x-display-name "") nil
                     (and (not display) (not (daemonp))))
  (setq x-display-name (car (x-display-list)))
  (wayland--watch-desktop-settings))

(cl-defmethod handle-args-function (args &context (window-system wayland))
  (x-handle-args args))

(cl-defmethod frame-creation-function (params &context (window-system wayland))
  (x-create-frame-with-faces params))

(defun wayland-device-class (name)
  "Return the device class of NAME.
Users should not call this function; see `device-class' instead.
Wayland does not identify input devices.  Scroll events name what
scrolled, as waylandterm.c gets it from the compositor."
  (cond
   ((not name) nil)
   ((string= name "Wayland Wheel Scrolling") 'mouse)
   ((string= name "Wayland Finger Scrolling") 'touchpad)
   ((string= name "Wayland Continuous Scrolling") 'trackpoint)
   ((string= name "Virtual core pointer") 'core-pointer)
   ((string= name "Virtual core keyboard") 'core-keyboard)))

;;; Desktop settings.

;; GTK takes its fonts and text scaling factor from GNOME's interface
;; settings, which the settings portal serves on GNOME and on other
;; desktops with the GNOME or GTK portal.  Read the same keys, so that
;; the default font and font sizes match PGTK's, and follow changes.

(declare-function wayland--set-desktop-settings "waylandfns.c"
                  (monospace normal text-scale))
(declare-function font-get-system-font "waylandfns.c" ())
(declare-function font-setting-change-default-font "dynamic-setting"
                  (display-or-frame set-font))
(declare-function dbus-call-method "dbus")
(declare-function dbus-register-signal "dbus")
(defvar font-use-system-font)

(defconst wayland--desktop-namespace "org.gnome.desktop.interface"
  "The settings namespace GTK reads its fonts and text scaling from.")

(defvar wayland--desktop-signal nil
  "The settings portal's change signal registration, or nil.")

(defun wayland--desktop-settings ()
  "Return the portal's interface settings as an alist, or nil."
  (and (featurep 'dbusbind)
       (ignore-errors
         (require 'dbus)
         (cadr (assoc wayland--desktop-namespace
                      (dbus-call-method
                       :session "org.freedesktop.portal.Desktop"
                       "/org/freedesktop/portal/desktop"
                       "org.freedesktop.portal.Settings" "ReadAll"
                       :timeout 1000
                       (list :array wayland--desktop-namespace)))))))

(defun wayland--read-desktop-settings ()
  "Pass the desktop's fonts and text scaling factor to the backend.
Return non-nil if the monospace font changed."
  (let* ((settings (wayland--desktop-settings))
         (value (lambda (key type)
                  (let ((value (car (cadr (assoc key settings)))))
                    (and (funcall type value) value))))
         (monospace (funcall value "monospace-font-name" #'stringp))
         (old (font-get-system-font)))
    (wayland--set-desktop-settings
     monospace (funcall value "font-name" #'stringp)
     (funcall value "text-scaling-factor" #'numberp))
    (not (equal monospace old))))

(defun wayland--desktop-setting-changed (namespace key &rest _)
  "Follow a change of KEY in NAMESPACE of the settings portal."
  (when (and (equal namespace wayland--desktop-namespace)
             (member key '("monospace-font-name" "font-name"
                           "text-scaling-factor"))
             (wayland--read-desktop-settings)
             font-use-system-font)
    (dolist (terminal (terminal-list))
      (when (eq (terminal-live-p terminal) 'wayland)
        (font-setting-change-default-font terminal t)))))

(defun wayland--watch-desktop-settings ()
  "Read the desktop's settings and follow their changes."
  (wayland--read-desktop-settings)
  (when (and (not wayland--desktop-signal) (featurep 'dbus))
    (setq wayland--desktop-signal
          (ignore-errors
            (dbus-register-signal
             :session "org.freedesktop.portal.Desktop"
             "/org/freedesktop/portal/desktop"
             "org.freedesktop.portal.Settings" "SettingChanged"
             #'wayland--desktop-setting-changed)))))

;;; Selections.

(declare-function wayland--selection-publishable-p "waylandselect.c"
                  (primary &optional frame))
(declare-function wayland--set-selection "waylandselect.c"
                  (primary payloads &optional frame))
(declare-function wayland--selection-owner-p "waylandselect.c"
                  (primary &optional frame))
(declare-function wayland--selection-targets "waylandselect.c"
                  (primary &optional frame))
(declare-function wayland--get-selection-mime "waylandselect.c"
                  (primary mime &optional frame))

(defconst wayland--utf8-mimes '("text/plain;charset=utf-8" "UTF8_STRING" "TEXT"))

(defun wayland--selection-payloads (selection value)
  (let* ((text (xselect-convert-to-string selection 'UTF8_STRING value))
         (utf8 (cdr-safe text))
         (payloads (when utf8
                     (mapcar (lambda (mime) (cons mime utf8))
                             (append wayland--utf8-mimes '("text/plain"))))))
    (when-let* ((latin (cdr-safe
                       (xselect-convert-to-string selection 'STRING value))))
      (push (cons "STRING" latin) payloads))
    (when (and (stringp value) (> (length value) 0))
      (let ((properties (text-properties-at 0 value)))
        (while properties
          (let ((type (pop properties))
                (bytes (pop properties)))
            (when (and (symbolp type) (stringp bytes)
                       (string-search "/" (symbol-name type)))
              (push (cons (symbol-name type)
                          (if (multibyte-string-p bytes)
                              (encode-coding-string bytes 'utf-8)
                            (copy-sequence bytes)))
                    payloads))))))
    (dolist (entry selection-converter-alist)
      (let* ((type (car entry))
             (mime (symbol-name type))
             (converter (cdr entry)))
        (when (and (string-search "/" mime) (not (assoc mime payloads))
                   (or (not (consp converter))
                       (funcall (car converter) selection type value)))
          (let ((converted (funcall (if (consp converter) (cdr converter)
                                     converter)
                                   selection type value)))
            (when (consp converted)
              (setq converted (cdr converted)))
            (when (stringp converted)
              (push (cons mime (if (multibyte-string-p converted)
                                   (encode-coding-string converted 'utf-8)
                                 (copy-sequence converted)))
                    payloads))))))
    payloads))

(cl-defmethod gui-backend-set-selection (selection value
                                         &context (window-system wayland))
  (when (memq selection '(CLIPBOARD PRIMARY))
    (let ((primary (eq selection 'PRIMARY)))
      (if (null value)
          (wayland--set-selection primary nil)
        (when (wayland--selection-publishable-p primary)
          (when-let* ((payloads (wayland--selection-payloads selection value)))
            (wayland--set-selection primary payloads)))))))

(cl-defmethod gui-backend-selection-owner-p (selection
                                             &context (window-system wayland))
  (and (memq selection '(CLIPBOARD PRIMARY))
       (wayland--selection-owner-p (eq selection 'PRIMARY))))

(cl-defmethod gui-backend-selection-exists-p (selection
                                              &context (window-system wayland))
  (and (memq selection '(CLIPBOARD PRIMARY))
       (not (null (wayland--selection-targets (eq selection 'PRIMARY))))))

(cl-defmethod gui-backend-get-selection (selection target
                                         &context (window-system wayland))
  (when (memq selection '(CLIPBOARD PRIMARY))
    (let* ((primary (eq selection 'PRIMARY))
           (mimes (wayland--selection-targets primary)))
      (if (eq target 'TARGETS)
          (when mimes
            (vconcat (mapcar #'intern mimes)
                     (when (seq-intersection mimes wayland--utf8-mimes)
                       '(UTF8_STRING TEXT))
                     (when (member "text/plain" mimes) '(STRING))))
        (let* ((preferred
                (cond ((memq target '(UTF8_STRING TEXT))
                       (append wayland--utf8-mimes '("text/plain")))
                      ((eq target 'STRING)
                       (cons "STRING" (append wayland--utf8-mimes '("text/plain"))))
                      (t (list (symbol-name target)))))
               (mime (seq-find (lambda (name) (member name mimes)) preferred)))
          (when-let* ((bytes (and mime (wayland--get-selection-mime primary mime))))
            (if (or (memq target '(UTF8_STRING TEXT STRING))
                    (member mime wayland--utf8-mimes))
                (prog1 (decode-coding-string
                        bytes (or next-selection-coding-system
                                  selection-coding-system
                                  (if (equal mime "STRING") 'iso-8859-1 'utf-8)))
                  (setq next-selection-coding-system nil))
              bytes)))))))

(defun wayland--menu-split (label)
  "Return LABEL's submenu path and its own name as a cons.
The path is nil for an item at the top of the menu."
  (if (string-match "\\`\\(.*\\) > " label)
      (cons (match-string 1 label) (substring label (match-end 0)))
    (cons nil label)))

(defun wayland--menu-groups-shown-p ()
  "Whether the completion UI shows groups rather than whole candidates."
  (or completions-group
      (bound-and-true-p vertico-mode)
      (bound-and-true-p icomplete-vertical-mode)
      (bound-and-true-p fido-vertical-mode)))

(defun wayland--popup-menu (frame title choices click)
  "Prompt on FRAME for one of the parsed menu CHOICES.
Items are grouped by their submenu, in menu order, and annotated with
their keyboard equivalents."
  (condition-case nil
      (with-selected-frame frame
        (let* ((seen (make-hash-table :test 'equal))
               (title (if (and (stringp title) (not (string-empty-p title)))
                          title "Menu"))
               (prompt (concat title ": ")))
          (setq choices
                (mapcar (lambda (entry)
                          (let* ((name (car entry))
                                 (label (copy-sequence name))
                                 (index 1))
                            (while (gethash label seen)
                              (setq label (format "%s (%d)" name (cl-incf index))))
                            (puthash label t seen)
                            ;; Menus pad the keyboard equivalent.
                            (let ((keys (get-text-property 0 'wayland-menu-keys
                                                           label)))
                              (when keys
                                (put-text-property 0 (length label)
                                                   'wayland-menu-keys
                                                   (string-trim keys) label)))
                            ;; Dim the path, for UIs that show it.
                            (let ((path (car (wayland--menu-split label))))
                              (when path
                                (add-face-text-property 0 (+ (length path) 3)
                                                        'shadow t label)))
                            (cons label (cdr entry))))
                        choices))
          (when choices
            (let* ((column
                    (+ 2 (apply #'max
                                (mapcar (lambda (choice)
                                          (string-width
                                           (if (wayland--menu-groups-shown-p)
                                               (cdr (wayland--menu-split (car choice)))
                                             (car choice))))
                                        choices))))
                   (metadata
                    `(metadata
                      (category . wayland-menu)
                      (display-sort-function . identity)
                      (cycle-sort-function . identity)
                      (group-function
                       . ,(lambda (candidate transform)
                            (let ((split (wayland--menu-split candidate)))
                              (if transform (cdr split) (or (car split) title)))))
                      (annotation-function
                       . ,(lambda (candidate)
                            (let ((keys (get-text-property 0 'wayland-menu-keys
                                                           candidate)))
                              (when keys
                                (concat (propertize " " 'display
                                                    `(space :align-to ,column))
                                        (propertize keys 'face
                                                    'completions-annotations))))))))
                   (table (lambda (string predicate action)
                            (if (eq action 'metadata)
                                metadata
                              (complete-with-action action choices string
                                                    predicate))))
                   (choice (assoc (completing-read prompt table nil t) choices)))
              (if choice (cdr choice) (signal 'quit nil))))))
    (quit (unless click (signal 'quit nil)))))

;;; Input methods.

(defvar wayland-preedit-overlay nil
  "The overlay showing the input method's preedit text, or nil.")

(defun wayland--preedit-face (index attributes)
  "Return the face for part INDEX of a preedit text, with ATTRIBUTES."
  (let ((face (intern (format "wayland-im-%d" index))))
    (unless (facep face)
      (make-face face))
    (face-spec-set face `((t . ,attributes)))
    face))

(defun wayland-preedit-text (event)
  "Show the preedit text of a `preedit-text' EVENT from the input method.
The text is shown before point in an overlay, which a later event
replaces or, with no text, removes.  This is the Wayland counterpart of
`pgtk-preedit-text'."
  (interactive "e")
  (when wayland-preedit-overlay
    (delete-overlay wayland-preedit-overlay)
    (setq wayland-preedit-overlay nil))
  (let ((index 0)
        (text ""))
    (dolist (part (nth 1 event))
      (let ((string (copy-sequence (car part)))
            attributes color)
        (when (setq color (cdr-safe (assq 'fg (cdr part))))
          (setq attributes (append attributes `(:foreground ,color))))
        (when (setq color (cdr-safe (assq 'bg (cdr part))))
          (setq attributes (append attributes `(:background ,color))))
        (when (setq color (cdr-safe (assq 'ul (cdr part))))
          (setq attributes (append attributes `(:underline ,color))))
        (add-text-properties 0 (length string)
                             `(face ,(wayland--preedit-face index attributes))
                             string)
        (setq text (concat text string)
              index (1+ index))))
    (unless (string-empty-p text)
      (let ((overlay (make-overlay (point) (point))))
        (overlay-put overlay 'before-string text)
        (setq wayland-preedit-overlay overlay)))))

(define-key special-event-map [preedit-text] #'wayland-preedit-text)

(provide 'wayland-win)

;;; wayland-win.el ends here
