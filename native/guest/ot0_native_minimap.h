// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Included after ptr/typed/serial/root in ot0_companion_tick.c. No native
// operation other than the source-proven base UWidget visibility setter.
typedef struct Ot0MmLease {
  uint32_t seq, desired, deadline, epoch, map_id;
  uint32_t canvas_index, canvas_serial, root_index, root_serial;
  uint64_t resource, section, canvas, root;
} Ot0MmLease;
typedef struct Ot0MmPrivate {
  uint32_t tag, owned_tag;
  Ot0MmLease committed;
  uint64_t owned_tree, owned_panel, owned_resource, owned_section;
  uint32_t owned_map_id;
} Ot0MmPrivate;
typedef struct Ot0MmContext {
  Addr root, root_slate, root_controller, tree, panel;
  Addr canvas, slate, controller, slot;
  uint32_t root_index, root_serial, index, serial;
  uint8_t visibility, slate_visibility, root_visibility, root_slate_visibility;
  uint8_t panel_visibility;
} Ot0MmContext;
_Static_assert(sizeof(Ot0MmPrivate) <= sizeof(((Ot0CompanionMailbox *)0)->reserved_tail),
               "minimap private cache fits mailbox tail");
_Static_assert(offsetof(Ot0CompanionMailbox, reserved_tail) % 8 == 0,
               "minimap private cache alignment");
#define OT0_MM_PRIVATE_TAG 0x4d4d4c31u
static int ot0_mm_visible(uint8_t visibility) {
  return visibility == 0 || visibility == 3 || visibility == 4;
}
static uint8_t ot0_mm_slate_visibility(Addr main, uint8_t visibility) {
  // Exact base setter1f137c8..1f13810:0 uses Visible;1..4 use the
  // native POINTER table. FVisibility constants may be only4-byte aligned.
  if (visibility > 4) return 0;
  Addr value = main + 0x6cf09dc;
  if (visibility) {
    const uint32_t offsets[4] = {0x6cf09e0, 0x6cf0934, 0x6cf0930, 0x6cf09e4};
    value = PTR(main, 0x5cfa588 + (Addr)(visibility - 1) * 8);
    if (value != main + offsets[visibility - 1]) return 0;
  }
  uint8_t result = U8(value, 0);
  return result == 1 || result == 2 || result == 4 || result == 0x11 || result == 0x19
           ? result : 0;
}
// Exact FName entry, never a name-pool/object search or name constructor.
static int ot0_mm_small_name(Addr main, Addr object) {
  uint64_t name = PTR(object, 0x18);
  if (name >> 32) return 0;
  uint32_t index = (uint32_t)name;
  if ((index >> 16) > 8191) return 0;
  Addr block = PTR(main, 0x6c91478 + (Addr)(index >> 16) * 8);
  if (!ptr(block)) return 0;
  Addr entry = block + (Addr)(index & 65535) * 2;
  if (entry >= ((Addr)1 << 39) - 64) return 0;
  uint16_t header = *(const uint16_t *)entry;
  const char expected[] = "CanvasPanel_MinimapSmall";
  if ((header & 1) || (header >> 6) != sizeof(expected) - 1) return 0;
  for (unsigned i = 0; i < sizeof(expected) - 1; ++i)
    if (U8(entry, 2 + i) != (uint8_t)expected[i]) return 0;
  return 1;
}
static int ot0_mm_lookup(Addr main, Ot0MmContext *out) {
  Addr ui = root(main);
  if (!ui) return 0;
  Addr root_slate = PTR(ui, 0x108), root_controller = PTR(ui, 0x110);
  Addr tree = PTR(ui, 0x278), panel = PTR(ui, 0x488);
  // UUserWidget.WidgetTree278; GetRootWidget1fd1fd4 -> tree.RootWidget30.
  if (!typed(main, tree, 0x5d0ee88) || !typed(main, panel, 0x5cc23b0) ||
      PTR(tree, 0x20) != ui || PTR(tree, 0x30) != panel ||
      PTR(panel, 0x20) != tree) return 0;
  Array slots;
  if (!array(panel + 0x178, &slots, 64) || slots.count < 1 || slots.count > 32)
    return 0;
  Addr entries[32], target = 0, target_slot = 0;
  for (int i = 0; i < slots.count; ++i) {
    Addr slot = PTR(slots.data, (Addr)i * 8);
    entries[i] = slot;
    if (!typed(main, slot, 0x5cc35a8) || PTR(slot, 0x28) != panel) return 0;
    Addr child = PTR(slot, 0x30);
    if (!ptr(child)) return 0;
    if (!typed(main, child, 0x5cc23b0) || !ot0_mm_small_name(main, child)) continue;
    if (target || PTR(child, 0x30) != slot || PTR(child, 0x20) != tree) return 0;
    target = child; target_slot = slot;
  }
  if (!target) return 0;
  Addr slate = PTR(target, 0x190), controller = PTR(target, 0x198);
  Addr cached = PTR(target, 0x108), weak = PTR(target, 0x110);
  // UCanvasPanel.Rebuild1eb5190 stores190/198. Base SetVisibility1f135f0
  // selects108/110, elsef8/100; both must identify this actual native Slate.
  if (!ptr(slate) || !ptr(controller) ||
      PTR(slate, 0) != main + 0x5b5af90 || PTR(slate, 8) != slate ||
      PTR(target, 0xf8) != slate || PTR(target, 0x100) != controller ||
      (cached && (cached != slate || weak != controller)) ||
      U32(controller, 8) < 1 || U32(controller, 8) > 1000000 ||
      U8(slate, 0x185) != 1) return 0;
  uint8_t visibility = U8(target, 0xdc), actual = U8(slate, 0x184);
  uint8_t root_visibility = U8(ui, 0xdc), panel_visibility = U8(panel, 0xdc);
  uint8_t root_actual = U8(root_slate, 0x184);
  if (visibility > 4 || root_visibility > 4 || panel_visibility > 4 ||
      (actual != 1 && actual != 2 && actual != 4 && actual != 0x11 && actual != 0x19) ||
      (root_actual != 1 && root_actual != 2 && root_actual != 4 &&
       root_actual != 0x11 && root_actual != 0x19)) return 0;
  uint32_t root_index, root_serial, index, number;
  if (!ensure_serial(main, ui, &root_index, &root_serial) ||
      !ensure_serial(main, target, &index, &number)) return 0;
  // Check current attachment, including replaced elements with unchanged
  // array storage/header. No cached raw UObject is trusted across ticks.
  if (!equal_array(slots, *(const Array *)(panel + 0x178)) ||
      PTR(ui, 0x278) != tree || PTR(ui, 0x488) != panel ||
      PTR(tree, 0x30) != panel || PTR(tree, 0x20) != ui ||
      PTR(target_slot, 0x28) != panel || PTR(target_slot, 0x30) != target ||
      PTR(target, 0x30) != target_slot || PTR(target, 0x20) != tree ||
      PTR(target, 0x190) != slate || PTR(target, 0x198) != controller ||
      U8(target, 0xdc) != visibility || U8(slate, 0x184) != actual ||
      U8(ui, 0xdc) != root_visibility || U8(panel, 0xdc) != panel_visibility ||
      U8(root_slate, 0x184) != root_actual ||
      PTR(ui, 0x108) != root_slate || PTR(ui, 0x110) != root_controller ||
      root(main) != ui) return 0;
  for (int i = 0; i < slots.count; ++i)
    if (PTR(slots.data, (Addr)i * 8) != entries[i]) return 0;
  uint32_t root_index2, root_serial2, index2, number2;
  if (!serial(main, ui, &root_index2, &root_serial2) || root_index2 != root_index ||
      root_serial2 != root_serial || !serial(main, target, &index2, &number2) ||
      index2 != index || number2 != number) return 0;
  out->root = ui; out->root_slate = root_slate;
  out->root_controller = root_controller; out->tree = tree; out->panel = panel;
  out->canvas = target; out->slate = slate; out->controller = controller;
  out->slot = target_slot; out->root_index = root_index; out->root_serial = root_serial;
  out->index = index; out->serial = number; out->visibility = visibility;
  out->slate_visibility = actual; out->root_visibility = root_visibility;
  out->root_slate_visibility = root_actual; out->panel_visibility = panel_visibility;
  return 1;
}
static int ot0_mm_same(const Ot0MmContext *a, const Ot0MmContext *b) {
  return a->root == b->root && a->root_slate == b->root_slate &&
    a->root_controller == b->root_controller && a->tree == b->tree &&
    a->panel == b->panel && a->canvas == b->canvas && a->slate == b->slate &&
    a->controller == b->controller && a->slot == b->slot &&
    a->root_index == b->root_index && a->root_serial == b->root_serial &&
    a->index == b->index && a->serial == b->serial;
}
// Source scope is independent of the host's current publication. A section
// transition invalidates an old cached lease even while a writer is odd.
static int ot0_mm_scope(Addr main, Addr resource, uint64_t section, uint32_t map_id) {
  Addr manager = PTR(main, 0x7cc7588);
  return resource && section && map_id && map_id <= 100000000 &&
    typed(main, manager, 0x63be0f0) && U32(manager, 0x348) == map_id &&
    PTR(manager, 0x448) == resource && ptr(resource) &&
    PTR(resource, 0) == section && U8(resource, 0xf8) == 1;
}
static Ot0MmLease ot0_mm_lease(volatile Ot0CompanionMailbox *m,
                              volatile Ot0MmPrivate *p) {
  Ot0MmLease none = {0};
  uint32_t seq = __atomic_load_n(&m->lease_seq, __ATOMIC_ACQUIRE);
  if (seq && !(seq & 1) &&
      (seq != p->committed.seq || p->committed.epoch != m->epoch)) {
    Ot0MmLease l = {0};
    l.seq = seq; l.desired = m->desired_hide; l.deadline = m->lease_deadline;
    l.epoch = m->lease_epoch; l.map_id = m->expected_map_id;
    l.resource = m->expected_resource; l.section = m->expected_section;
    l.canvas = m->expected_canvas; l.canvas_index = m->expected_canvas_index;
    l.canvas_serial = m->expected_canvas_serial; l.root = m->expected_root;
    l.root_index = m->expected_root_index; l.root_serial = m->expected_root_serial;
    if (__atomic_load_n(&m->lease_seq, __ATOMIC_ACQUIRE) == seq)
      p->committed = l;
  }
  // An odd/torn writer cannot refresh the last committed deadline.
  if (!p->committed.seq || (p->committed.seq & 1)) return none;
  return p->committed;
}
static int ot0_mm_wanted(volatile Ot0CompanionMailbox *m, Addr main,
                         uint32_t state, const Ot0MmLease *l,
                         const Ot0MmContext *c) {
  Addr app = PTR(main, 0x7cc8cc0), menus = PTR(main, 0x7cc9a18);
  Addr manager = PTR(main, 0x7cc7588);
  uint32_t remaining = l->deadline - m->heartbeat;
  return l->seq && !(l->seq & 1) && l->desired == 1 && l->epoch == m->epoch &&
    (int32_t)remaining >= 0 && remaining <= 120 && !m->relocation_delta &&
    (state == 23 || state == 24) && typed(main, app, 0x6207610) &&
    U8(app, 0x2f0) == state && typed(main, menus, 0x63b6f18) && U8(menus, 0x38a) == 0 &&
    ot0_mm_visible(c->root_visibility) && ot0_mm_visible(c->panel_visibility) &&
    c->root_slate_visibility == ot0_mm_slate_visibility(main, c->root_visibility) &&
    l->root == c->root && l->root_index == c->root_index && l->root_serial == c->root_serial &&
    l->canvas == c->canvas && l->canvas_index == c->index && l->canvas_serial == c->serial &&
    ot0_mm_scope(main, l->resource, l->section, l->map_id) &&
    typed(main, manager, 0x63be0f0) && U8(manager, 0x7b0) == 0;
}
static void ot0_mm_clear(volatile Ot0CompanionMailbox *m, volatile Ot0MmPrivate *p) {
  m->owned_ui = 0; m->owned_slate = 0; m->owned_controller = 0;
  m->owned_index = 0; m->owned_serial = 0; m->original_visibility = 0;
  m->owned_root = 0; m->owned_root_slate = 0; m->owned_root_controller = 0;
  m->owned_root_index = 0; m->owned_root_serial = 0; m->owned_slot = 0;
  m->hidden = 0; p->owned_tag = 0;
  p->owned_tree = 0; p->owned_panel = 0;
  p->owned_resource = 0; p->owned_section = 0; p->owned_map_id = 0;
}
static int ot0_mm_owned(volatile Ot0CompanionMailbox *m,
                       volatile Ot0MmPrivate *p, const Ot0MmContext *c) {
  return p->owned_tag == 1 && m->owned_ui == c->canvas &&
    m->owned_slate == c->slate && m->owned_controller == c->controller &&
    m->owned_index == c->index && m->owned_serial == c->serial &&
    m->owned_root == c->root && m->owned_root_slate == c->root_slate &&
    m->owned_root_controller == c->root_controller &&
    m->owned_root_index == c->root_index && m->owned_root_serial == c->root_serial &&
    m->owned_slot == c->slot && p->owned_tree == c->tree && p->owned_panel == c->panel;
}
static void ot0_mm_take(volatile Ot0CompanionMailbox *m, volatile Ot0MmPrivate *p,
                       const Ot0MmContext *c, const Ot0MmLease *l, uint8_t original) {
  m->owned_ui = c->canvas; m->owned_slate = c->slate; m->owned_controller = c->controller;
  m->owned_index = c->index; m->owned_serial = c->serial;
  m->original_visibility = original;
  m->owned_root = c->root; m->owned_root_slate = c->root_slate;
  m->owned_root_controller = c->root_controller;
  m->owned_root_index = c->root_index; m->owned_root_serial = c->root_serial;
  m->owned_slot = c->slot; p->owned_tree = c->tree; p->owned_panel = c->panel;
  p->owned_resource = l->resource; p->owned_section = l->section;
  p->owned_map_id = l->map_id; p->owned_tag = 1; m->hidden = 1;
}
static void ot0_native_minimap_tick(volatile Ot0CompanionMailbox *m,
                                   uint64_t main, uint32_t appstate) {
  volatile Ot0MmPrivate *p = (volatile Ot0MmPrivate *)(void *)m->reserved_tail;
  if (p->tag != OT0_MM_PRIVATE_TAG) {
    p->committed.seq = 0; p->owned_tag = 0; p->tag = OT0_MM_PRIVATE_TAG;
  }
  __atomic_add_fetch(&m->status_seq, 1, __ATOMIC_ACQ_REL);
  Ot0MmLease lease = ot0_mm_lease(m, p);
  Ot0MmContext c;
  int have = ot0_mm_lookup(main, &c), attempted = 0;
  int want = have && ot0_mm_wanted(m, main, appstate, &lease, &c);
  if (m->owned_ui) {
    if (!have || !ot0_mm_owned(m, p, &c) || c.visibility != 1) {
      // Recycled/detached objects and native game visibility overrides are
      // not ours. Never force a restoration onto a different live object.
      ot0_mm_clear(m, p);
    } else if (!want || !ot0_mm_scope(main, p->owned_resource,
                                    p->owned_section, p->owned_map_id)) {
      uint8_t original = (uint8_t)m->original_visibility;
      if (ot0_mm_visible(original)) FN(0x1f135f0, void, Addr, uint8_t)(c.canvas, original);
      ot0_mm_clear(m, p);
      have = ot0_mm_lookup(main, &c);
    } else if (c.slate_visibility == 2) {
      m->hidden = 1;
    } else {
      // A native repaint changed only Slate state. Reapply through the real
      // setter, then verify actual collapsed state before claiming hidden.
      attempted = 1;
      FN(0x1f135f0, void, Addr, uint8_t)(c.canvas, 1);
      Ot0MmContext after;
      if (!ot0_mm_lookup(main, &after) || !ot0_mm_same(&c, &after) ||
          after.visibility != 1 || after.slate_visibility != 2) {
        if (ot0_mm_lookup(main, &after) && ot0_mm_same(&c, &after) && after.visibility == 1 &&
            ot0_mm_visible((uint8_t)m->original_visibility))
          FN(0x1f135f0, void, Addr, uint8_t)(after.canvas, (uint8_t)m->original_visibility);
        ot0_mm_clear(m, p);
      } else { c = after; m->hidden = 1; }
    }
  }
  if (!m->owned_ui && !attempted) {
    lease = ot0_mm_lease(m, p);
    have = ot0_mm_lookup(main, &c);
    want = have && ot0_mm_wanted(m, main, appstate, &lease, &c);
    if (want && ot0_mm_visible(c.visibility) &&
        c.slate_visibility == ot0_mm_slate_visibility(main, c.visibility)) {
      uint8_t original = c.visibility;
      FN(0x1f135f0, void, Addr, uint8_t)(c.canvas, 1);
      Ot0MmContext after;
      if (ot0_mm_lookup(main, &after) && ot0_mm_same(&c, &after) &&
          after.visibility == 1 && after.slate_visibility == 2) {
        ot0_mm_take(m, p, &after, &lease, original);
      } else if (ot0_mm_lookup(main, &after) && ot0_mm_same(&c, &after) && after.visibility == 1) {
        FN(0x1f135f0, void, Addr, uint8_t)(after.canvas, original);
      }
    }
  }
  // A setter notification or concurrent newly committed host lease can
  // change context. Recheck before publishing a claimed hidden state.
  if (m->owned_ui) {
    lease = ot0_mm_lease(m, p);
    have = ot0_mm_lookup(main, &c);
    if (!have || !ot0_mm_owned(m, p, &c) || c.visibility != 1) {
      ot0_mm_clear(m, p);
    } else if (!ot0_mm_wanted(m, main, appstate, &lease, &c) ||
               !ot0_mm_scope(main, p->owned_resource, p->owned_section, p->owned_map_id)) {
      uint8_t original = (uint8_t)m->original_visibility;
      if (ot0_mm_visible(original)) FN(0x1f135f0, void, Addr, uint8_t)(c.canvas, original);
      ot0_mm_clear(m, p);
    } else m->hidden = c.slate_visibility == 2;
  }
  have = ot0_mm_lookup(main, &c);
  m->native_visibility = have ? c.visibility : 255;
  __atomic_add_fetch(&m->status_seq, 1, __ATOMIC_RELEASE);
}
