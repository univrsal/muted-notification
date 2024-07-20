#include "indicator.h"

#include <obs-frontend-api.h>
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QCursor>
#include <QScreen>
#include <QTimer>
#include <QTime>

class OverlayWidget : public QWidget {
	QTimer *m_timer;
	bool m_want_show = false;
	int m_timeout_ms = 0;
	int m_indicator_size = 0;
	uint64_t m_start_time = 0;
public:
	OverlayWidget(QWidget *parent)
		: QWidget(parent)
	{
		setWindowFlags(Qt::FramelessWindowHint |
			       Qt::WindowStaysOnTopHint | Qt::Tool);
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_ShowWithoutActivating);

		// Set fixed size for the indicator
		setFixedSize(50, 50);
		setVisible(false);
		m_timer = new QTimer(this);

		connect(m_timer, &QTimer::timeout, this, &OverlayWidget::tick);
		m_timer->start(100);
	}

	void queue_show(int timeout, int size)
	{
		m_timeout_ms = timeout;
		m_indicator_size = size;
		m_want_show = true;
	}

protected:
	void updateIndicatorPosition()
	{
		QPoint cursorPos = QCursor::pos();
		QScreen *screen = QGuiApplication::screenAt(cursorPos);
		if (screen) {
			QRect screenGeometry = screen->geometry();
			move(screenGeometry.topLeft() + QPoint(10, 10));
		}
	}

	void paintEvent(QPaintEvent *event) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setBrush(QBrush(Qt::red));
		painter.drawEllipse(0, 0, width(), height());
	}

protected slots:

	void tick()
	{
		if (m_want_show) {
			m_start_time = QDateTime::currentMSecsSinceEpoch();
			m_want_show = false;
			setFixedSize(m_indicator_size, m_indicator_size);
			updateIndicatorPosition();
			setVisible(true);
		}

		if (m_timeout_ms > 0) {
			uint64_t current_time =
				QDateTime::currentMSecsSinceEpoch();
			if (current_time - m_start_time > m_timeout_ms) {
				setVisible(false);
				m_timeout_ms = 0;
			}
		}
	}
};

static OverlayWidget *overlay = nullptr;

void indicator_init()
{
	QWidget* main_window = static_cast<QWidget*>(obs_frontend_get_main_window());
	if (!main_window)
		return;

	overlay = new OverlayWidget(main_window);
}

void indicator_show(int timeout_ms, int indicator_size)
{
	if (overlay) {
		overlay->queue_show(timeout_ms, indicator_size);
	}
}
