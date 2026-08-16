/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PULSEVIEW_PV_VIEWS_TRACE_VIEWWIDGET_HPP
#define PULSEVIEW_PV_VIEWS_TRACE_VIEWWIDGET_HPP

#include <memory>

#include <QPoint>
#include <QWidget>

#include <pv/util.hpp>

using std::shared_ptr;
using std::vector;

class QTouchEvent;
class QWidget;

namespace pv {
namespace views {
namespace trace {

class View;
class ViewItem;

class ViewWidgetBase
{
protected:
	ViewWidgetBase(View &parent, QWidget &widget);

	virtual void item_hover(const shared_ptr<ViewItem> &item, QPoint pos);
	virtual void item_clicked(const shared_ptr<ViewItem> &item);
	virtual void selection_changed_event() = 0;

	bool accept_drag() const;
	bool mouse_down() const;
	void drag_items(const QPoint &delta);

	virtual void drag();
	virtual void drag_by(const QPoint &delta);
	virtual void drag_release();

	virtual vector< shared_ptr<ViewItem> > items() = 0;
	virtual shared_ptr<ViewItem> get_mouse_over_item(const QPoint &pt) = 0;

	void mouse_left_press_event(QMouseEvent *event);
	void mouse_left_release_event(QMouseEvent *event);
	virtual bool touch_event(QTouchEvent *event);

	bool handle_event(QEvent *event);
	void handle_mouse_press_event(QMouseEvent *event);
	void handle_mouse_release_event(QMouseEvent *event);
	void handle_mouse_move_event(QMouseEvent *event);
	void handle_key_press_event(QKeyEvent *event);
	void handle_key_release_event(QKeyEvent *event);
	void handle_leave_event(QEvent *event);
	void clear_selection_items();

protected:
	QWidget &widget_;
	pv::views::trace::View &view_;
	QPoint mouse_point_;
	QPoint mouse_down_point_;
	pv::util::Timestamp mouse_down_offset_;
	shared_ptr<ViewItem> mouse_down_item_;
	Qt::KeyboardModifiers mouse_modifiers_;
	bool item_dragging_;
};

class ViewWidget : public QWidget, protected ViewWidgetBase
{
	Q_OBJECT

protected:
	ViewWidget(View &parent);
	void selection_changed_event() override;

protected:
	bool event(QEvent *event);

	void mousePressEvent(QMouseEvent *event);
	void mouseReleaseEvent(QMouseEvent *event);
	void mouseMoveEvent(QMouseEvent *event);

	void keyPressEvent(QKeyEvent *event);
	void keyReleaseEvent(QKeyEvent *event);

	void leaveEvent(QEvent *event);

public Q_SLOTS:
	void clear_selection();

Q_SIGNALS:
	void selection_changed();

};

} // namespace trace
} // namespace views
} // namespace pv

#endif // PULSEVIEW_PV_VIEWS_TRACE_VIEWWIDGET_HPP
