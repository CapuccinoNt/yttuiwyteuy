#!/usr/bin/env python3
"""
flight_sim.py - offline test bench for ARIA's helicopter flight controller.

This is a line-by-line Python port of Scripts/Game/ARIA/Aviation/ARIA_FlightController.c
(+ the parts of ARIA_HeliAirframe / ARIA_HeliPilot that build commands), flown on a
simple rigid-body model:

  * Enfusion conventions: Y up, X right, Z forward, world-space angular velocity (rad/s)
  * gravity, linear drag, and an UNKNOWN "native rotor lift" the controller is not told
    about (it must be absorbed by the velocity integral, like in game)
  * fixed physics step, controller run every rendered frame (variable frame time)

It does not prove the mod works in Reforger, but it proves the control law is stable,
signs are consistent, and the mission phases (takeoff, transit, hover, orbit, landing)
converge with the default gains.

Usage:  python3 tools/flight_sim.py [--model velocity|forces] [--plot]
"""
import math
import random
import sys

G = 9.81

# ---------------------------------------------------------------- vector helpers
def add(a, b): return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
def sub(a, b): return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
def mul(a, k): return [a[0] * k, a[1] * k, a[2] * k]
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def length(a): return math.sqrt(dot(a, a))
def flatten(a): return [a[0], 0.0, a[2]]
def hlen(a): return math.sqrt(a[0] * a[0] + a[2] * a[2])
def clamp(v, lo, hi): return max(lo, min(hi, v))
UP = [0.0, 1.0, 0.0]
FWD = [0.0, 0.0, 1.0]


def safe_norm(v, fb):
    l = length(v)
    return fb if l < 1e-4 else mul(v, 1 / l)


def clamp_len(v, m):
    l = length(v)
    return v if l <= m or l < 1e-4 else mul(v, m / l)


def clamp_h(v, m):
    l = hlen(v)
    if l <= m or l < 1e-4:
        return v
    k = m / l
    return [v[0] * k, v[1], v[2] * k]


def angle_between(a, b):
    la, lb = length(a), length(b)
    if la < 1e-4 or lb < 1e-4:
        return 0.0
    return math.degrees(math.acos(clamp(dot(a, b) / (la * lb), -1, 1)))


def limit_angle_from(ref, d, max_deg):
    r = safe_norm(ref, UP)
    d = safe_norm(d, r)
    if angle_between(r, d) <= max_deg:
        return d
    ortho = sub(d, mul(r, dot(d, r)))
    if length(ortho) < 1e-4:
        return r
    ortho = mul(ortho, 1 / length(ortho))
    rad = math.radians(max_deg)
    return add(mul(r, math.cos(rad)), mul(ortho, math.sin(rad)))


def yaw_from_dir(d): return math.degrees(math.atan2(d[0], d[2]))
def dir_from_yaw(y): return [math.sin(math.radians(y)), 0.0, math.cos(math.radians(y))]
def right_of(d): return [d[2], 0.0, -d[0]]


def wrap180(a):
    while a >= 180: a -= 360
    while a < -180: a += 360
    return a


def smooth_factor(rate, dt): return clamp(1 - math.exp(-rate * dt), 0, 1)
def arrival_speed(dist, decel): return 0.0 if dist <= 0 else math.sqrt(2 * decel * dist)


# ---------------------------------------------------------------- settings (defaults of ARIA_HeliSettings)
class Cfg:
    cruise = 170 / 3.6
    combat = 110 / 3.6
    orbit_speed = 90 / 3.6
    max_tilt = 28.0
    max_climb = 9.0
    max_descent = 7.0
    landing_rate = 2.0
    max_yaw_rate = 45.0
    max_accel = 4.5
    vel_gain = 0.9
    vel_int = 0.25
    alt_gain = 0.6
    att_gain = 3.5
    yaw_gain = 1.6
    ang_resp = 6.0
    h_assist = 0.25
    max_thrust_g = 2.0


# ---------------------------------------------------------------- port of ARIA_PID / ARIA_VectorPID
class PID:
    def __init__(self, kp, ki, kd, ilim, olim):
        self.kp, self.ki, self.kd, self.ilim, self.olim = kp, ki, kd, ilim, olim
        self.i = 0.0
        self.last = None

    def reset(self):
        self.i = 0.0
        self.last = None

    def update(self, sp, meas, dt):
        if dt <= 0:
            return 0.0
        e = sp - meas
        self.i = clamp(self.i + e * dt, -self.ilim, self.ilim)
        d = 0.0 if self.last is None else -(meas - self.last) / dt
        self.last = meas
        return clamp(self.kp * e + self.ki * self.i + self.kd * d, -self.olim, self.olim)


class VPID:
    def __init__(self, kp, ki, kd, il, ol):
        self.p = [PID(kp, ki, kd, il, ol) for _ in range(3)]

    def set_vertical(self, kp, ki, kd, il, ol):
        self.p[1] = PID(kp, ki, kd, il, ol)

    def reset(self):
        for p in self.p: p.reset()

    def update(self, sp, m, dt):
        return [self.p[k].update(sp[k], m[k], dt) for k in range(3)]


# ---------------------------------------------------------------- port of ARIA_FlightController
class Command:
    def __init__(self):
        self.active = True
        self.velocity_mode = False
        self.target = [0, 0, 0]
        self.des_vel = [0, 0, 0]
        self.target_alt = 0.0
        self.max_speed = 30.0
        self.decel = 2.5
        self.max_climb = 8.0
        self.max_descent = 6.0
        self.direct_vs = False
        self.vs = 0.0
        self.heading = 'VELOCITY'
        self.look_at = [0, 0, 0]


class Controller:
    MAX_TILT_RATE = 1.2

    def __init__(self, c):
        self.c = c
        self.vpid = VPID(c.vel_gain, c.vel_int, 0, 6, c.max_accel * 2)
        self.vpid.set_vertical(c.vel_gain * 1.4, c.vel_int * 2.0, 0, 12, G * 2)
        self.held_yaw = None

    def reset(self):
        self.vpid.reset()
        self.held_yaw = None

    def update(self, st, cmd, dt):
        c = self.c
        dv = self.horiz_vel(st, cmd)
        dv[1] = self.vert_speed(st, cmd)
        accel = clamp_h(self.vpid.update(dv, st.vel, dt), c.max_accel)
        required = mul(add(accel, mul(UP, G)), st.mass)
        des_up = limit_angle_from(UP, required, c.max_tilt)
        thrust = clamp(dot(required, st.up), 0, c.max_thrust_g * G * st.mass)
        impulse = mul(st.up, thrust * dt)
        if c.h_assist > 0:
            impulse = add(impulse, mul(flatten(accel), st.mass * c.h_assist * dt))
        tilt_axis = cross(st.up, des_up)
        omega_tilt = clamp_len(mul(tilt_axis, c.att_gain), self.MAX_TILT_RATE)
        yaw_err = math.radians(self.yaw_error(st, cmd, dv))
        mr = math.radians(c.max_yaw_rate)
        omega_yaw = mul(UP, clamp(yaw_err * c.yaw_gain, -mr, mr))
        omega_cmd = add(omega_tilt, omega_yaw)
        k = smooth_factor(c.ang_resp, dt)
        omega = add(st.omega, mul(sub(omega_cmd, st.omega), k))
        return omega, impulse, accel, angle_between(st.up, des_up)

    def horiz_vel(self, st, cmd):
        if cmd.velocity_mode:
            return clamp_h(flatten(cmd.des_vel), cmd.max_speed)
        to = flatten(sub(cmd.target, st.pos))
        d = length(to)
        if d < 0.05:
            return [0, 0, 0]
        sp = min(cmd.max_speed, arrival_speed(d, cmd.decel))
        sp = min(sp, d * 0.8)
        return mul(to, sp / d)

    def vert_speed(self, st, cmd):
        if cmd.direct_vs:
            return clamp(cmd.vs, -cmd.max_descent, cmd.max_climb)
        vz = (cmd.target_alt - st.pos[1]) * self.c.alt_gain
        agl = st.pos[1] - st.ground
        md = cmd.max_descent
        if agl < 40:
            md = min(md, max(1.5, agl * 0.15))
        return clamp(vz, -md, cmd.max_climb)

    def yaw_error(self, st, cmd, dv):
        heading = safe_norm(flatten(st.fwd), FWD)
        cur = yaw_from_dir(heading)
        tgt = cur
        if cmd.heading == 'VELOCITY':
            f = flatten(dv)
            if length(f) > 4:
                tgt = yaw_from_dir(f)
                self.held_yaw = tgt
            elif self.held_yaw is not None:
                tgt = self.held_yaw
        elif cmd.heading == 'HOLD':
            if self.held_yaw is None:
                self.held_yaw = cur
            tgt = self.held_yaw
        elif cmd.heading == 'TARGET':
            t = flatten(sub(cmd.look_at, st.pos))
            if length(t) > 1:
                tgt = yaw_from_dir(t)
        return wrap180(tgt - cur)


# ---------------------------------------------------------------- rigid body "game"
class Body:
    def __init__(self, mass=2400.0):
        self.mass = mass
        self.pos = [0.0, 1.0, 0.0]
        self.vel = [0.0, 0.0, 0.0]
        self.omega = [0.0, 0.0, 0.0]
        self.right = [1.0, 0.0, 0.0]
        self.up = [0.0, 1.0, 0.0]
        self.fwd = [0.0, 0.0, 1.0]
        self.ground = 0.0
        self.on_ground = True

    def orthonormalize(self):
        f = safe_norm(self.fwd, FWD)
        u = safe_norm(sub(self.up, mul(f, dot(self.up, f))), UP)
        self.right = cross(u, f)
        self.up, self.fwd = u, f

    def step(self, dt, native_lift, drag, wind):
        # gravity + unknown native rotor lift along body up + drag + wind
        a = [0.0, -G, 0.0]
        a = add(a, mul(self.up, native_lift))
        rel = sub(self.vel, wind)
        a = add(a, mul(rel, -drag))
        self.vel = add(self.vel, mul(a, dt))
        for name in ('right', 'up', 'fwd'):
            v = getattr(self, name)
            setattr(self, name, add(v, mul(cross(self.omega, v), dt)))
        self.orthonormalize()
        self.pos = add(self.pos, mul(self.vel, dt))
        # ground contact
        if self.pos[1] <= self.ground + 1.0:
            self.pos[1] = self.ground + 1.0
            if self.vel[1] < 0:
                self.vel[1] = 0
            self.vel[0] *= 0.9
            self.vel[2] *= 0.9
            self.on_ground = True
        else:
            self.on_ground = False


class State:
    pass


def read_state(b):
    s = State()
    s.pos, s.vel, s.omega = b.pos[:], b.vel[:], b.omega[:]
    s.up, s.fwd, s.right = b.up[:], b.fwd[:], b.right[:]
    s.mass, s.ground = b.mass, b.ground
    return s


# ---------------------------------------------------------------- mission script
def run(model='velocity', seed=1, verbose=True):
    random.seed(seed)
    cfg = Cfg()
    body = Body()
    ctl = Controller(cfg)
    native_lift = G * 0.35          # unknown to the controller
    drag = 0.04
    wind = [3.0, 0.0, -2.0]
    phys_dt = 1 / 60.0
    t = 0.0
    log = []
    phase = 'TAKEOFF'
    phase_t = 0.0
    takeoff_pos = body.pos[:]
    target = [2500.0, 0.0, 1800.0]
    orbit_center = [2500.0, 0.0, 2600.0]
    lz = [2600.0, 0.0, 2000.0]
    results = {}
    max_tilt = 0.0
    max_rate = 0.0

    def agl():
        return body.pos[1] - 1.0 - body.ground

    while t < 420:
        frame_dt = random.uniform(1 / 75, 1 / 30)   # variable frame rate
        st = read_state(body)
        cmd = Command()
        cmd.max_climb, cmd.max_descent, cmd.decel = cfg.max_climb, cfg.max_descent, 2.2

        if phase == 'TAKEOFF':
            cmd.target, cmd.max_speed = takeoff_pos, 3
            cmd.direct_vs, cmd.vs, cmd.heading = True, 3.5, 'HOLD'
            if agl() >= 15:
                phase, phase_t = 'TRANSIT', t
                ctl.reset()
        elif phase == 'TRANSIT':
            cmd.target, cmd.max_speed = target, cfg.cruise
            cmd.target_alt = body.ground + 120
            if hlen(sub(target, body.pos)) < 60 and hlen(body.vel) < 6:
                results['transit_time'] = t - phase_t
                phase, phase_t = 'HOVER', t
        elif phase == 'HOVER':
            cmd.target, cmd.max_speed, cmd.heading = target, 15, 'HOLD'
            cmd.target_alt = body.ground + 120
            if t - phase_t > 20:
                results['hover_error'] = hlen(sub(target, body.pos))
                results['hover_alt_error'] = abs(body.pos[1] - 120)
                phase, phase_t = 'ORBIT', t
        elif phase == 'ORBIT':
            R = 450.0
            radial = flatten(sub(body.pos, orbit_center))
            d = length(radial)
            if d > R * 1.8:
                entry = add(orbit_center, mul(safe_norm(radial, FWD), R))
                to = safe_norm(flatten(sub(entry, body.pos)), FWD)
                cmd.velocity_mode, cmd.des_vel, cmd.max_speed = True, mul(to, cfg.combat), cfg.combat
            else:
                rd = safe_norm(radial, FWD)
                tangent = right_of(rd)
                corr = clamp((R - d) * 0.25, -cfg.orbit_speed * 0.6, cfg.orbit_speed * 0.6)
                cmd.velocity_mode = True
                cmd.des_vel = add(mul(tangent, cfg.orbit_speed), mul(rd, corr))
                cmd.max_speed = cfg.orbit_speed * 1.3
            cmd.target_alt = body.ground + 140
            if t - phase_t > 60:
                errs = [abs(hlen(sub(p, orbit_center)) - R) for (tt, p, ph) in log if ph == 'ORBIT' and tt > phase_t + 40]
                results['orbit_radius_error_max'] = max(errs) if errs else None
                phase, phase_t = 'APPROACH', t
        elif phase == 'APPROACH':
            dist = hlen(sub(lz, body.pos))
            tt = clamp((dist - 150) / 650, 0, 1)
            cmd.target, cmd.max_speed = lz, cfg.cruise
            cmd.target_alt = body.ground + 30 + (120 - 30) * tt
            if dist < 200:
                cmd.decel = 1.6
            if dist < 20 and hlen(body.vel) < 5:
                phase, phase_t = 'LANDING', t
                ctl.reset()
        elif phase == 'LANDING':
            a = max(agl(), 0)
            cmd.target, cmd.max_speed = lz, 4
            cmd.direct_vs, cmd.heading = True, 'HOLD'
            cmd.vs = -clamp(a * 0.35, cfg.landing_rate, cfg.max_descent * 0.6)
            if a < 3:
                cmd.vs = -cfg.landing_rate * 0.6
            if body.on_ground and t - phase_t > 1:
                results['landing_error'] = hlen(sub(lz, body.pos))
                results['touchdown_vs'] = body.vel[1]
                results['landing_time'] = t - phase_t
                phase = 'LANDED'
        elif phase == 'LANDED':
            break

        omega, impulse, accel, tilt_err = ctl.update(st, cmd, frame_dt)
        # apply like ARIA_HeliAirframe.Apply
        if model == 'forces':
            body.vel = add(body.vel, mul(impulse, 1 / body.mass))
        else:
            body.vel = add(add(st.vel, mul(accel, frame_dt)), mul(UP, G * frame_dt))
        body.omega = omega
        max_rate = max(max_rate, math.degrees(length(omega)))
        tilt = angle_between(body.up, UP)
        if phase not in ('TAKEOFF', 'LANDING'):
            max_tilt = max(max_tilt, tilt)

        # physics sub-steps for this frame
        remaining = frame_dt
        while remaining > 1e-6:
            h = min(phys_dt, remaining)
            body.step(h, native_lift, drag, wind)
            remaining -= h
        t += frame_dt
        log.append((t, body.pos[:], phase))

        if body.pos[1] > 2000 or math.isnan(body.pos[1]):
            results['diverged'] = True
            break

    results['final_phase'] = phase
    results['max_tilt_deg'] = round(max_tilt, 1)
    results['max_rate_deg_s'] = round(max_rate, 1)
    results['sim_time'] = round(t, 1)
    if verbose:
        for k, v in results.items():
            print(f'  {k:24s} {v if not isinstance(v, float) else round(v, 2)}')
    return results


def main():
    model = 'velocity'
    if '--model' in sys.argv:
        model = sys.argv[sys.argv.index('--model') + 1]
    ok = True
    for seed in (1, 2, 3):
        print(f'== model={model} seed={seed}')
        r = run(model, seed)
        passed = (r.get('final_phase') == 'LANDED' and not r.get('diverged')
                  and r.get('landing_error', 99) < 6 and r.get('hover_error', 99) < 10
                  and (r.get('orbit_radius_error_max') or 0) < 60
                  and r['max_tilt_deg'] <= Cfg.max_tilt + 5)
        print('  RESULT', 'PASS' if passed else 'FAIL')
        ok &= passed
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
