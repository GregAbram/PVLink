using UnityEngine;
using UnityEngine.InputSystem;

namespace ParaViewLink
{
    /// <summary>
    /// Attach to any GameObject. Left-drag rotates it around the world origin.
    /// Alt+left-drag or middle-drag pans the camera.
    /// Right-drag or scroll wheel zooms the camera.
    /// </summary>
    public class OrbitRotator : MonoBehaviour
    {
        [Header("Rotation")]
        public float RotateSpeed = 0.3f;

        [Header("Zoom")]
        public float ZoomSpeed   = 2.0f;
        public float MinDistance = 0.5f;
        public float MaxDistance = 500f;

        [Header("Pan")]
        public float PanSpeed = 0.005f;

        // ----------------------------------------------------------------

        private Camera  _cam;
        private Vector2 _lastMouse;

        private void Awake()
        {
            _cam = Camera.main;
        }

        private void Update()
        {
            var mouse = Mouse.current;
            if (mouse == null) return;

            Vector2 pos   = mouse.position.ReadValue();
            Vector2 delta = pos - _lastMouse;
            _lastMouse    = pos;

            bool altHeld  = Keyboard.current != null &&
                            (Keyboard.current.leftAltKey.isPressed ||
                             Keyboard.current.rightAltKey.isPressed);

            bool rotateBtn = mouse.leftButton.isPressed   && !altHeld;
            bool panBtn    = mouse.middleButton.isPressed ||
                             (mouse.leftButton.isPressed  && altHeld);
            bool zoomBtn   = mouse.rightButton.isPressed;

            if (rotateBtn) Rotate(delta);
            if (panBtn)    Pan(delta);
            if (zoomBtn)   Zoom(delta.y);

            Vector2 scroll = mouse.scroll.ReadValue();
            if (scroll.sqrMagnitude > 0.001f)
                Zoom(scroll.y * 0.1f);
        }

        // ----------------------------------------------------------------

        private void Rotate(Vector2 delta)
        {
            transform.RotateAround(Vector3.zero, Vector3.up,           -delta.x * RotateSpeed);
            transform.RotateAround(Vector3.zero, _cam.transform.right,  delta.y * RotateSpeed);
        }

        private void Pan(Vector2 delta)
        {
            if (_cam == null) return;
            float   dist = Vector3.Distance(_cam.transform.position, Vector3.zero);
            Vector3 move = (-_cam.transform.right * delta.x
                           + -_cam.transform.up   * delta.y)
                           * PanSpeed * dist;
            _cam.transform.position += move;
        }

        private void Zoom(float amount)
        {
            if (_cam == null) return;
            Vector3 dir     = _cam.transform.forward;
            float   dist    = Vector3.Distance(_cam.transform.position, Vector3.zero);
            float   newDist = Mathf.Clamp(dist - amount * ZoomSpeed * (dist * 0.1f),
                                          MinDistance, MaxDistance);
            _cam.transform.position = -dir * newDist;
        }
    }
}
