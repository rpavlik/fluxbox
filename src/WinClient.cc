// WinClient.cc for Fluxbox - an X11 Window manager
// Copyright (c) 2003 - 2006 Henrik Kinnunen (fluxgen at fluxbox dot org)
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

#include "WinClient.hh"

#include "Window.hh"
#include "fluxbox.hh"
#include "FocusControl.hh"
#include "Screen.hh"
#include "FbAtoms.hh"

#include "Xutil.hh"

#include "FbTk/EventManager.hh"
#include "FbTk/MultLayers.hh"

#include <iostream>
#include <algorithm>
#include <iterator>
#include <memory>
#include <X11/Xatom.h>

#ifdef HAVE_CASSERT
  #include <cassert>
#else
  #include <assert.h>
#endif
#ifdef HAVE_CSTRING
  #include <cstring>
#else
  #include <string.h>
#endif

using std::string;
using std::list;
using std::mem_fun;

#ifdef DEBUG
using std::cerr;
using std::endl;
using std::hex;
using std::dec;
#endif // DEBUG

WinClient::TransientWaitMap WinClient::s_transient_wait;

WinClient::WinClient(Window win, BScreen &screen, FluxboxWindow *fbwin):
        Focusable(screen, fbwin), FbTk::FbWindow(win),
                     transient_for(0),
                     window_group(0),
                     x(0), y(0), old_bw(0),
                     initial_state(0),
                     normal_hint_flags(0),
                     wm_hint_flags(0),
                     m_modal_count(0),
                     m_modal(false),
                     accepts_input(false),
                     send_focus_message(false),
                     send_close_message(false),
                     m_win_gravity(0),
                     m_title_override(false),
                     m_icon_override(false),
                     m_window_type(Focusable::TYPE_NORMAL),
                     m_mwm_hint(0),
                     m_strut(0) {

    m_size_hints.min_width = m_size_hints.min_height =
        m_size_hints.width_inc = m_size_hints.height_inc =
        m_size_hints.base_width = m_size_hints.base_height = 1;

    m_size_hints.max_width = m_size_hints.max_height =
        m_size_hints.min_aspect_x = m_size_hints.min_aspect_y =
        m_size_hints.max_aspect_x = m_size_hints.max_aspect_y = 0;

    updateWMProtocols();
    updateMWMHints();
    updateWMHints();
    updateWMNormalHints();
    updateWMClassHint();
    updateTitle();
    Fluxbox::instance()->saveWindowSearch(win, this);
    if (window_group != None)
        Fluxbox::instance()->saveGroupSearch(window_group, this);

    // search for this in transient waiting list
    if (s_transient_wait.find(win) != s_transient_wait.end()) {
        // Found transients that are waiting for this.
        // For each transient that waits call updateTransientInfo
        for_each(s_transient_wait[win].begin(),
                 s_transient_wait[win].end(),
                 mem_fun(&WinClient::updateTransientInfo));
        // clear transient waiting list for this window
        s_transient_wait.erase(win);
    }

    // also check if this window is a transient
    // this needs to be done before creating an fbwindow, so this doesn't get
    // tabbed using the apps file
    updateTransientInfo();
}

WinClient::~WinClient() {
#ifdef DEBUG
    cerr<<__FILE__<<"(~"<<__FUNCTION__<<")[this="<<this<<"]"<<endl;
#endif // DEBUG

    FbTk::EventManager::instance()->remove(window());

    clearStrut();

    //
    // clear transients and transient_for
    //
    if (transient_for != 0) {
        assert(transient_for != this);
        transient_for->transientList().remove(this);
        if (m_modal)
            transient_for->removeModal();
        transient_for = 0;
    }

    while (!transients.empty()) {
        transients.back()->transient_for = 0;
        transients.pop_back();
    }

    accepts_input = send_focus_message = false;
    if (fbwindow() != 0)
        fbwindow()->removeClient(*this);

    // this takes care of any focus issues
    m_diesig.notify();

    Fluxbox *fluxbox = Fluxbox::instance();

    // This fixes issue 1 (see WinClient.hh):
    // If transients die before the transient_for is created
    removeTransientFromWaitingList();
    s_transient_wait.erase(window());


    if (window_group != 0) {
        fluxbox->removeGroupSearch(window_group);
        window_group = 0;
    }

    if (m_mwm_hint != 0)
        XFree(m_mwm_hint);

    if (window())
        fluxbox->removeWindowSearch(window());
}

bool WinClient::acceptsFocus() const {
    return ((accepts_input || send_focus_message) &&
            // focusing fbpanel messes up quite a few things
            m_window_type != Focusable::TYPE_DOCK &&
            m_window_type != Focusable::TYPE_SPLASH);
}

bool WinClient::sendFocus() {
    if (accepts_input) {
        setInputFocus(RevertToPointerRoot, CurrentTime);
        FocusControl::setExpectingFocus(this);
        return true;
    }
    if (!send_focus_message)
        return false;
#ifdef DEBUG
    cerr<<"WinClient::"<<__FUNCTION__<<": this = "<<this<<
        " window = 0x"<<hex<<window()<<dec<<endl;
#endif // DEBUG

    // setup focus msg
    XEvent ce;
    ce.xclient.type = ClientMessage;
    ce.xclient.message_type = FbAtoms::instance()->getWMProtocolsAtom();
    ce.xclient.display = display();
    ce.xclient.window = window();
    ce.xclient.format = 32;
    ce.xclient.data.l[0] = FbAtoms::instance()->getWMTakeFocusAtom();
    ce.xclient.data.l[1] = Fluxbox::instance()->getLastTime();
    ce.xclient.data.l[2] = 0l;
    ce.xclient.data.l[3] = 0l;
    ce.xclient.data.l[4] = 0l;
    // send focus msg
    XSendEvent(display(), window(), false, NoEventMask, &ce);
    FocusControl::setExpectingFocus(this);
    return true;
}

void WinClient::sendClose(bool forceful) {
    if (forceful || !send_close_message)
        XKillClient(display(), window());
    else {
        // send WM_DELETE message
        // fill in XClientMessage structure for delete message
        XEvent ce;
        ce.xclient.type = ClientMessage;
        ce.xclient.message_type = FbAtoms::instance()->getWMProtocolsAtom();
        ce.xclient.display = display();
        ce.xclient.window = window();
        ce.xclient.format = 32;
        ce.xclient.data.l[0] = FbAtoms::instance()->getWMDeleteAtom();
        ce.xclient.data.l[1] = CurrentTime;
        ce.xclient.data.l[2] = 0l;
        ce.xclient.data.l[3] = 0l;
        ce.xclient.data.l[4] = 0l;
        // send event delete message to client window
        XSendEvent(display(), window(), false, NoEventMask, &ce);
    }
}

bool WinClient::getAttrib(XWindowAttributes &attr) const {
    return XGetWindowAttributes(display(), window(), &attr);
}

bool WinClient::getWMName(XTextProperty &textprop) const {
    return XGetWMName(display(), window(), &textprop);
}

bool WinClient::getWMIconName(XTextProperty &textprop) const {
    return XGetWMIconName(display(), window(), &textprop);
}

string WinClient::getWMRole() const {
    Atom wm_role = XInternAtom(FbTk::App::instance()->display(),
                               "WM_WINDOW_ROLE", False);
    return textProperty(wm_role);
}

void WinClient::updateWMClassHint() {
    XClassHint ch;
    if (XGetClassHint(display(), window(), &ch) == 0) {
#ifdef DEBUG
        cerr<<"WinClient: Failed to read class hint!"<<endl;
#endif //DEBUG
        m_instance_name = m_class_name = "";
    } else {

        if (ch.res_name != 0) {
            m_instance_name = const_cast<char *>(ch.res_name);
            XFree(ch.res_name);
            ch.res_name = 0;
        } else
            m_instance_name = "";

        if (ch.res_class != 0) {
            m_class_name = const_cast<char *>(ch.res_class);
            XFree(ch.res_class);
            ch.res_class = 0;
        } else
            m_class_name = "";
    }
}

void WinClient::updateTransientInfo() {
    // remove this from parent
    if (transientFor() != 0) {
        transientFor()->transientList().remove(this);
        if (m_modal)
            transientFor()->removeModal();
    }

    transient_for = 0;
    // determine if this is a transient window
    Window win = 0;
    if (!XGetTransientForHint(display(), window(), &win)) {
#ifdef DEBUG
        cerr<<__FUNCTION__<<": window() = 0x"<<hex<<window()<<dec<<"Failed to read transient for hint."<<endl;
#endif // DEBUG
        return;
    }

    // we can't be transient to ourself
    if (win == window()) {
#ifdef DEBUG
        cerr<<__FUNCTION__<<": transient to ourself"<<endl;
#endif // DEBUG
        return;
    }

    if (win != None && screen().rootWindow() == win) {
        // transient for root window... =  transient for group
        // I don't think we are group-aware yet
        return;
    }


    transient_for = Fluxbox::instance()->searchWindow(win);
    // if we did not find a transient WinClient but still
    // have a transient X window, then we have to put the
    // X transient_for window in a waiting list and update this clients transient
    // list later when the transient_for has a Winclient
    if (!transient_for) {
        // We might also already waiting for an old transient_for;
        //
        // this call fixes issue 2:
        // If transients changes to new transient_for before the old transient_for is created.
        // (see comment in WinClient.hh)
        //
        removeTransientFromWaitingList();

        s_transient_wait[win].push_back(this);
    }


#ifdef DEBUG
    cerr<<__FUNCTION__<<": transient_for window = 0x"<<hex<<win<<dec<<endl;
    cerr<<__FUNCTION__<<": transient_for = "<<transient_for<<endl;
#endif // DEBUG
    // make sure we don't have deadlock loop in transient chain
    for (WinClient *w = this; w != 0; w = w->transient_for) {
        if (this == w->transient_for)
            w->transient_for = 0;
    }

    if (transientFor() != 0) {
        // we need to add ourself to the right client in
        // the transientFor() window so we search client
        transient_for->transientList().push_back(this);
        if (m_modal)
            transient_for->addModal();
    }

}


void WinClient::updateTitle() {
    // why 512? very very long wmnames seem to either
    // crash fluxbox or to make it have high cpuload
    // see also:
    //    http://www.securityfocus.com/archive/1/382398/2004-11-24/2004-11-30/2
    //
    // TODO: - find out why this mostly happens when using xft-fonts
    //       - why other windowmanagers (pekwm/pwm3/openbox etc) are
    //         also influenced
    //
    // the limitation to 512 chars only avoids running in that trap
    if (m_title_override)
        return;

    m_title = string(Xutil::getWMName(window()), 0, 512);
    titleSig().notify();
}

void WinClient::setTitle(const FbTk::FbString &title) {
    m_title = title;
    m_title_override = true;
    titleSig().notify();
}

void WinClient::setIcon(const FbTk::PixmapWithMask& pm) {

    m_icon.pixmap().copy(pm.pixmap());
    m_icon.mask().copy(pm.mask());
    m_icon_override = true;
    titleSig().notify();
}

void WinClient::saveBlackboxAttribs(FluxboxWindow::BlackboxAttributes &blackbox_attribs, int nelements) {
    changeProperty(FbAtoms::instance()->getFluxboxAttributesAtom(),
                   XA_CARDINAL, 32, PropModeReplace,
                   (unsigned char *)&blackbox_attribs,
                   nelements);
}

void WinClient::setFluxboxWindow(FluxboxWindow *win) {
    m_fbwin = win;
}

void WinClient::updateMWMHints() {
    int format;
    Atom atom_return;
    unsigned long num = 0, len = 0;

    if (m_mwm_hint) {
        XFree(m_mwm_hint);
        m_mwm_hint = 0;
    }
    Atom motif_wm_hints = FbAtoms::instance()->getMWMHintsAtom();

    if (!(property(motif_wm_hints, 0,
                   PropMwmHintsElements, false,
                   motif_wm_hints, &atom_return,
                   &format, &num, &len,
                   (unsigned char **) &m_mwm_hint) &&
          m_mwm_hint)) {
        if (num != static_cast<unsigned int>(PropMwmHintsElements)) {
            XFree(m_mwm_hint);
            m_mwm_hint = 0;
            return;
        }
    }
}

void WinClient::updateWMHints() {
    XWMHints *wmhint = XGetWMHints(display(), window());
    accepts_input = true;
    window_group = None;
    initial_state = NormalState;
    if (wmhint) {
        wm_hint_flags = wmhint->flags;
        /*
         * ICCCM 4.1.7
         *---------------------------------------------
         * Input Model      Input Field   WM_TAKE_FOCUS
         *---------------------------------------------
         * No Input          False         Absent
         * Passive           True          Absent
         * Locally Active    True          Present
         * Globally Active   False         Present
         *---------------------------------------------
         * Here: WM_TAKE_FOCUS = send_focus_message
         *       Input Field   = accepts_input
         */
        if (wmhint->flags & InputHint)
            accepts_input = (bool)wmhint->input;

        if (wmhint->flags & StateHint)
            initial_state = wmhint->initial_state;

        if (wmhint->flags & WindowGroupHint && !window_group)
            window_group = wmhint->window_group;

        if (! m_icon_override) {

            if ((bool)(wmhint->flags & IconPixmapHint) && wmhint->icon_pixmap != 0)
                m_icon.pixmap().copy(wmhint->icon_pixmap, 0, 0);
            else
                m_icon.pixmap().release();

            if ((bool)(wmhint->flags & IconMaskHint) && wmhint->icon_mask != 0)
                m_icon.mask().copy(wmhint->icon_mask, 0, 0);
            else
                m_icon.mask().release();
        }

        if (fbwindow()) {
            if (wmhint->flags & XUrgencyHint) {
                Fluxbox::instance()->attentionHandler().addAttention(*this);
            } else {
                Fluxbox::instance()->attentionHandler().
                    update(&m_focussig);
            }
        }

        XFree(wmhint);
    }
}


void WinClient::updateWMNormalHints() {
    long icccm_mask;
    XSizeHints sizehint;
    if (! XGetWMNormalHints(display(), window(), &sizehint, &icccm_mask)) {
        m_size_hints.min_width = m_size_hints.min_height =
            m_size_hints.base_width = m_size_hints.base_height =
            m_size_hints.width_inc = m_size_hints.height_inc = 1;
        m_size_hints.max_width = 0; // unbounded
        m_size_hints.max_height = 0;
        m_size_hints.min_aspect_x = m_size_hints.min_aspect_y =
            m_size_hints.max_aspect_x = m_size_hints.max_aspect_y = 0;
        m_win_gravity = NorthWestGravity;
    } else {
        normal_hint_flags = sizehint.flags;

        if (sizehint.flags & PMinSize) {
            m_size_hints.min_width = sizehint.min_width;
            m_size_hints.min_height = sizehint.min_height;
            if (!(sizehint.flags & PBaseSize)) {
                m_size_hints.base_width = m_size_hints.min_width;
                m_size_hints.base_height = m_size_hints.min_height;
            }
        } else {
            m_size_hints.min_width = m_size_hints.min_height = 1;
            m_size_hints.base_width = m_size_hints.base_height = 0;
        }

        if (sizehint.flags & PBaseSize) {
            m_size_hints.base_width = sizehint.base_width;
            m_size_hints.base_height = sizehint.base_height;
            if (!(sizehint.flags & PMinSize)) {
                m_size_hints.min_width = m_size_hints.base_width;
                m_size_hints.min_height = m_size_hints.base_height;
            }
        } // default set in PMinSize

        if (sizehint.flags & PMaxSize) {
            m_size_hints.max_width = sizehint.max_width;
            m_size_hints.max_height = sizehint.max_height;
        } else {
            m_size_hints.max_width = 0; // unbounded
            m_size_hints.max_height = 0;
        }

        if (sizehint.flags & PResizeInc) {
            m_size_hints.width_inc = sizehint.width_inc;
            m_size_hints.height_inc = sizehint.height_inc;
        } else
            m_size_hints.width_inc = m_size_hints.height_inc = 1;

        if (m_size_hints.width_inc == 0)
            m_size_hints.width_inc = 1;
        if (m_size_hints.height_inc == 0)
            m_size_hints.height_inc = 1;

        if (sizehint.flags & PAspect) {
            m_size_hints.min_aspect_x = sizehint.min_aspect.x;
            m_size_hints.min_aspect_y = sizehint.min_aspect.y;
            m_size_hints.max_aspect_x = sizehint.max_aspect.x;
            m_size_hints.max_aspect_y = sizehint.max_aspect.y;
        } else
            m_size_hints.min_aspect_x = m_size_hints.min_aspect_y =
                m_size_hints.max_aspect_x = m_size_hints.max_aspect_y = 0;

        if (sizehint.flags & PWinGravity)
            m_win_gravity = sizehint.win_gravity;
        else
            m_win_gravity = NorthWestGravity;

    }
}

Window WinClient::getGroupLeftWindow() const {
    int format;
    Atom atom_return;
    unsigned long num = 0, len = 0;
    Atom group_left_hint = XInternAtom(display(), "_FLUXBOX_GROUP_LEFT", False);

    Window *data = 0;
    if (property(group_left_hint, 0,
                   1, false,
                   XA_WINDOW, &atom_return,
                   &format, &num, &len,
                   (unsigned char **) &data) &&
        data) {
        if (num != 1) {
            XFree(data);
            return None;
        } else {
            Window ret = *data;
            XFree(data);
            return ret;
        }
    }
    return None;
}


void WinClient::setGroupLeftWindow(Window win) {
    if (m_screen.isShuttingdown())
        return;
    Atom group_left_hint = XInternAtom(display(), "_FLUXBOX_GROUP_LEFT", False);
    changeProperty(group_left_hint, XA_WINDOW, 32,
                   PropModeReplace, (unsigned char *) &win, 1);
}

bool WinClient::hasGroupLeftWindow() const {
    // try to find _FLUXBOX_GROUP_LEFT atom in window
    // if we have one then we have a group left window
    int format;
    Atom atom_return;
    unsigned long num = 0, len = 0;
    Atom group_left_hint = XInternAtom(display(), "_FLUXBOX_GROUP_LEFT", False);

    Window *data = 0;
    if (property(group_left_hint, 0,
                   1, false,
                   XA_WINDOW, &atom_return,
                   &format, &num, &len,
                   (unsigned char **) &data) &&
        data) {
            XFree(data);
            if (num != 1)
                return false;
            else
                return true;
    }

    return false;
}

void WinClient::setStateModal(bool state) {
    if (state == m_modal)
        return;

    m_modal = state;
    if (transient_for) {
        if (state)
            transient_for->addModal();
        else
            transient_for->removeModal();
    }

    // TODO: we're not implementing the following part of EWMH spec:
    // "if WM_TRANSIENT_FOR is not set or set to the root window the dialog is
    //  modal for its window group."
}

bool WinClient::validateClient() const {
    FbTk::App::instance()->sync(false);

    XEvent e;
    if (( XCheckTypedWindowEvent(display(), window(), DestroyNotify, &e) ||
          XCheckTypedWindowEvent(display(), window(), UnmapNotify, &e))
        && XPutBackEvent(display(), &e)) {
        Fluxbox::instance()->ungrab();
        return false;
    }

    return true;
}

void WinClient::setStrut(Strut *strut) {
    clearStrut();
    m_strut = strut;
}

void WinClient::clearStrut() {
    if (m_strut != 0) {
        screen().clearStrut(m_strut);
        screen().updateAvailableWorkspaceArea();
        m_strut = 0;
    }
}

bool WinClient::focus() {
    if (fbwindow() == 0)
        return false;
    else
        return fbwindow()->setCurrentClient(*this, true);
}

bool WinClient::isFocused() const {
    return (fbwindow() ?
        fbwindow()->isFocused() && &fbwindow()->winClient() == this :
        false);
}

void WinClient::setAttentionState(bool value) {
    Focusable::setAttentionState(value);
    if (fbwindow() && !fbwindow()->isFocused())
        fbwindow()->setAttentionState(value);
}

void WinClient::updateWMProtocols() {
    Atom *proto = 0;
    int num_return = 0;
    FbAtoms *fbatoms = FbAtoms::instance();

    if (XGetWMProtocols(display(), window(), &proto, &num_return)) {

        // defaults
        send_focus_message = false;
        send_close_message = false;
        for (int i = 0; i < num_return; ++i) {
            if (proto[i] == fbatoms->getWMDeleteAtom())
                send_close_message = true;
            else if (proto[i] == fbatoms->getWMTakeFocusAtom())
                send_focus_message = true;
        }

        XFree(proto);
        if (fbwindow())
            fbwindow()->updateFunctions();
#ifdef DEBUG
    } else {
        cerr<<"Warning: Failed to read WM Protocols. "<<endl;
#endif // DEBUG
    }

}

/* For aspect ratios
   Note that its slightly simplified in that only the
   line gradient is given - this is because for aspect
   ratios, we always have the line going through the origin

   * Based on this formula:
   http://astronomy.swin.edu.au/~pbourke/geometry/pointline/

   Note that a gradient from origin goes through ( grad , 1 )
 */

void closestPointToLine(double &ret_x, double &ret_y,
                        double point_x, double point_y,
                        double gradient) {
    double u = (point_x * gradient + point_y) /
        (gradient*gradient + 1);

    ret_x = u*gradient;
    ret_y = u;
}

/**
 * Changes width and height to the nearest (lower) value
 * that conforms to it's size hints.
 *
 * display_* give the values that would be displayed
 * to the user when resizing.
 * We use pointers for display_* since they are optional.
 *
 * See ICCCM section 4.1.2.3
 */
void WinClient::applySizeHints(int &width, int &height,
                               int *display_width, int *display_height,
                               bool maximizing) {

    int i = width, j = height;

    // Check minimum size
    if (width < 0 || width < static_cast<signed>(m_size_hints.min_width))
        width = m_size_hints.min_width;

    if (height < 0 || height < static_cast<signed>(m_size_hints.min_height))
        height = m_size_hints.min_height;

    // Check maximum size
    if (m_size_hints.max_width > 0 && width > static_cast<signed>(m_size_hints.max_width))
        width = m_size_hints.max_width;

    if (m_size_hints.max_height > 0 && height > static_cast<signed>(m_size_hints.max_height))
        height = m_size_hints.max_height;

    // we apply aspect ratios before incrementals
    // Too difficult to exactly satisfy both incremental+aspect
    // in most situations
    // (they really shouldn't happen at the same time anyway).

    /* aspect ratios are applied exclusive to the m_size_hints.base_width
     *
     * m_size_hints.min_aspect_x      width      m_size_hints.max_aspect_x
     * ------------  <  -------  <  ------------
     * m_size_hints.min_aspect_y      height     m_size_hints.max_aspect_y
     *
     * beware of integer maximum (so I'll use doubles instead and divide)
     *
     * The trick is how to get back to the aspect ratio with minimal
     * change - do we modify x, y or both?
     * A: we minimise the distance between the current point, and
     *    the target aspect ratio (consider them as x,y coordinates)
     *  Consider that the aspect ratio is a line, and the current
     *  w/h is a point, so we're just using the formula for
     *  shortest distance from a point to a line!
     *
     * When maximizing, we must not increase any of the sizes, because we
     * would end up with the window partly off a screen, so a simpler formula
     * is used in that case.
     */

    if (m_size_hints.min_aspect_y > 0 && m_size_hints.max_aspect_y > 0 &&
        (height - m_size_hints.base_height) > 0) {
        double widthd = static_cast<double>(width - m_size_hints.base_width);
        double heightd = static_cast<double>(height - m_size_hints.base_height);

        double min = static_cast<double>(m_size_hints.min_aspect_x) /
            static_cast<double>(m_size_hints.min_aspect_y);

        double max = static_cast<double>(m_size_hints.max_aspect_x) /
            static_cast<double>(m_size_hints.max_aspect_y);

        double actual = widthd / heightd;

        if (max > 0 && min > 0 && actual > 0) { // don't even try otherwise
            bool changed = false;
            if (actual < min) {
                changed = true;
                if (maximizing)
                    heightd = widthd / min;
                else
                    closestPointToLine(widthd, heightd, widthd, heightd, min);
            } else if (actual > max) {
                changed = true;
                if (maximizing)
                    widthd = heightd * max;
                else
                    closestPointToLine(widthd, heightd, widthd, heightd, max);
            }

            if (changed) {
                width = static_cast<int>(widthd) + m_size_hints.base_width;
                height = static_cast<int>(heightd) + m_size_hints.base_height;
            }
        }
    }

    // enforce incremental size limits, wrt base size
    // only calculate this if we really need to
    i = (width - static_cast<signed>(m_size_hints.base_width)) /
        static_cast<signed>(m_size_hints.width_inc);
    width = i*static_cast<signed>(m_size_hints.width_inc) +
        static_cast<signed>(m_size_hints.base_width);

    j = (height - static_cast<signed>(m_size_hints.base_height)) /
        static_cast<signed>(m_size_hints.height_inc);
    height = j*static_cast<signed>(m_size_hints.height_inc) +
        static_cast<signed>(m_size_hints.base_height);

    if (display_width)
        *display_width = i;

    if (display_height)
        *display_height = j;
}

// check if the given width and height satisfy the size hints
bool WinClient::checkSizeHints(unsigned int width, unsigned int height) {
    if (width < m_size_hints.min_width || height < m_size_hints.min_height)
        return false;

    if (width > m_size_hints.max_width || height > m_size_hints.max_height)
        return false;

    if ((width - m_size_hints.base_width) % m_size_hints.width_inc != 0)
        return false;

    if ((height - m_size_hints.base_height) % m_size_hints.height_inc != 0)
        return false;

    double ratio = (double)width / (double)height;

    if (m_size_hints.min_aspect_y > 0 && (double)m_size_hints.min_aspect_x / (double)m_size_hints.min_aspect_y > ratio)
        return false;

    if (m_size_hints.max_aspect_y > 0 && (double)m_size_hints.max_aspect_x / (double)m_size_hints.max_aspect_y < ratio)
        return false;

    return true;
}

void WinClient::removeTransientFromWaitingList() {

    // holds the windows that dont have empty
    // transient waiting list
    list<Window> remove_list;

    // The worst case complexity is huge, but since we usually do not (virtualy never)
    // have a large transient waiting list the time spent here is neglectable
    TransientWaitMap::iterator t_it = s_transient_wait.begin();
    TransientWaitMap::iterator t_it_end = s_transient_wait.end();
    for (; t_it != t_it_end; ++t_it) {
        (*t_it).second.remove(this);
        // if the list is empty, add it to remove list
        // so we can erase it later
        if ((*t_it).second.empty())
            remove_list.push_back((*t_it).first);
    }

    // erase empty waiting lists
    list<Window>::iterator it = remove_list.begin();
    list<Window>::iterator it_end = remove_list.end();
    for (; it != it_end; ++it)
        s_transient_wait.erase(*it);
}
