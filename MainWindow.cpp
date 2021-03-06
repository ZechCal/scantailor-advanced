/*
    Scan Tailor - Interactive post-processing tool for scanned pages.
    Copyright (C)  Joseph Artsimovich <joseph.artsimovich@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "MainWindow.h"
#include <QDir>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QMessageBox>
#include <QResource>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStackedLayout>
#include <boost/lambda/lambda.hpp>
#include "AbstractRelinker.h"
#include "Application.h"
#include "AutoRemovingFile.h"
#include "BasicImageView.h"
#include "CommandLine.h"
#include "ContentBoxPropagator.h"
#include "DebugImageView.h"
#include "DebugImages.h"
#include "DefaultParamsDialog.h"
#include "ErrorWidget.h"
#include "FilterOptionsWidget.h"
#include "FixDpiDialog.h"
#include "ImageInfo.h"
#include "ImageMetadataLoader.h"
#include "LoadFileTask.h"
#include "LoadFilesStatusDialog.h"
#include "NewOpenProjectPanel.h"
#include "OutOfMemoryDialog.h"
#include "OutOfMemoryHandler.h"
#include "PageOrientationPropagator.h"
#include "PageSelectionAccessor.h"
#include "PageSequence.h"
#include "ProcessingIndicationWidget.h"
#include "ProcessingTaskQueue.h"
#include "ProjectCreationContext.h"
#include "ProjectOpeningContext.h"
#include "ProjectPages.h"
#include "ProjectReader.h"
#include "ProjectWriter.h"
#include "RecentProjects.h"
#include "RelinkingDialog.h"
#include "ScopedIncDec.h"
#include "SettingsDialog.h"
#include "SkinnedButton.h"
#include "SmartFilenameOrdering.h"
#include "StageSequence.h"
#include "SystemLoadWidget.h"
#include "TabbedDebugImages.h"
#include "ThumbnailFactory.h"
#include "UnitsProvider.h"
#include "Utils.h"
#include "WorkerThreadPool.h"
#include "filters/deskew/CacheDrivenTask.h"
#include "filters/deskew/Task.h"
#include "filters/fix_orientation/CacheDrivenTask.h"
#include "filters/fix_orientation/Task.h"
#include "filters/output/CacheDrivenTask.h"
#include "filters/output/TabbedImageView.h"
#include "filters/output/Task.h"
#include "filters/page_layout/CacheDrivenTask.h"
#include "filters/page_layout/Task.h"
#include "filters/page_split/CacheDrivenTask.h"
#include "filters/page_split/Task.h"
#include "filters/select_content/CacheDrivenTask.h"
#include "filters/select_content/Task.h"
#include "ui_AboutDialog.h"
#include "ui_BatchProcessingLowerPanel.h"
#include "ui_RemovePagesDialog.h"
#include "version.h"

class MainWindow::PageSelectionProviderImpl : public PageSelectionProvider {
 public:
  PageSelectionProviderImpl(MainWindow* wnd) : m_wnd(wnd) {}

  virtual PageSequence allPages() const { return m_wnd ? m_wnd->allPages() : PageSequence(); }

  virtual std::set<PageId> selectedPages() const { return m_wnd ? m_wnd->selectedPages() : std::set<PageId>(); }

  std::vector<PageRange> selectedRanges() const { return m_wnd ? m_wnd->selectedRanges() : std::vector<PageRange>(); }

 private:
  QPointer<MainWindow> m_wnd;
};


MainWindow::MainWindow()
    : m_pages(new ProjectPages),
      m_stages(new StageSequence(m_pages, newPageSelectionAccessor())),
      m_workerThreadPool(new WorkerThreadPool),
      m_interactiveQueue(new ProcessingTaskQueue()),
      m_outOfMemoryDialog(new OutOfMemoryDialog),
      m_curFilter(0),
      m_ignoreSelectionChanges(0),
      m_ignorePageOrderingChanges(0),
      m_debug(false),
      m_closing(false) {
  QSettings app_settings;

  m_maxLogicalThumbSize = app_settings.value("settings/max_logical_thumb_size", QSize(250, 160)).toSizeF();
  m_thumbSequence = std::make_unique<ThumbnailSequence>(m_maxLogicalThumbSize);

  m_autoSaveTimer.setSingleShot(true);
  connect(&m_autoSaveTimer, SIGNAL(timeout()), SLOT(autoSaveProject()));

  setupUi(this);
  sortOptions->setVisible(false);

  createBatchProcessingWidget();
  m_processingIndicationWidget.reset(new ProcessingIndicationWidget);

  filterList->setStages(m_stages);
  filterList->selectRow(0);

  setupThumbView();  // Expects m_thumbSequence to be initialized.
  m_tabbedDebugImages.reset(new TabbedDebugImages);

  m_debug = actionDebug->isChecked();

  m_imageFrameLayout = new QStackedLayout(imageViewFrame);
  m_imageFrameLayout->setStackingMode(QStackedLayout::StackAll);

  m_optionsFrameLayout = new QStackedLayout(filterOptions);

  m_statusBarPanel = std::make_unique<StatusBarPanel>();
  QMainWindow::statusBar()->addPermanentWidget(m_statusBarPanel.get());
  connect(m_thumbSequence.get(), &ThumbnailSequence::newSelectionLeader, [this](const PageInfo& page_info) {
    PageSequence pageSequence = m_thumbSequence->toPageSequence();
    if (pageSequence.numPages() > 0) {
      m_statusBarPanel->updatePage(pageSequence.pageNo(page_info.id()) + 1, pageSequence.numPages(), page_info.id());
    } else {
      m_statusBarPanel->clear();
    }
  });

  m_unitsMenuActionGroup = std::make_unique<QActionGroup>(this);
  for (QAction* action : menuUnits->actions()) {
    m_unitsMenuActionGroup->addAction(action);
  }
  switch (unitsFromString(QSettings().value("settings/units", "mm").toString())) {
    case PIXELS:
      actionPixels->setChecked(true);
      break;
    case MILLIMETRES:
      actionMilimeters->setChecked(true);
      break;
    case CENTIMETRES:
      actionCentimetres->setChecked(true);
      break;
    case INCHES:
      actionInches->setChecked(true);
      break;
  }
  connect(actionPixels, &QAction::toggled, [this](bool checked) {
    if (checked) {
      UnitsProvider::getInstance()->setUnits(PIXELS);
      QSettings().setValue("settings/units", unitsToString(PIXELS));
    }
  });
  connect(actionMilimeters, &QAction::toggled, [this](bool checked) {
    if (checked) {
      UnitsProvider::getInstance()->setUnits(MILLIMETRES);
      QSettings().setValue("settings/units", unitsToString(MILLIMETRES));
    }
  });
  connect(actionCentimetres, &QAction::toggled, [this](bool checked) {
    if (checked) {
      UnitsProvider::getInstance()->setUnits(CENTIMETRES);
      QSettings().setValue("settings/units", unitsToString(CENTIMETRES));
    }
  });
  connect(actionInches, &QAction::toggled, [this](bool checked) {
    if (checked) {
      UnitsProvider::getInstance()->setUnits(INCHES);
      QSettings().setValue("settings/units", unitsToString(INCHES));
    }
  });

  addAction(actionFirstPage);
  addAction(actionLastPage);
  addAction(actionNextPage);
  addAction(actionPrevPage);
  addAction(actionPrevPageQ);
  addAction(actionNextPageW);
  addAction(actionNextSelectedPage);
  addAction(actionPrevSelectedPage);
  addAction(actionNextSelectedPageW);
  addAction(actionPrevSelectedPageQ);

  addAction(actionSwitchFilter1);
  addAction(actionSwitchFilter2);
  addAction(actionSwitchFilter3);
  addAction(actionSwitchFilter4);
  addAction(actionSwitchFilter5);
  addAction(actionSwitchFilter6);
  // Should be enough to save a project.
  OutOfMemoryHandler::instance().allocateEmergencyMemory(3 * 1024 * 1024);

  connect(actionFirstPage, SIGNAL(triggered(bool)), SLOT(goFirstPage()));
  connect(actionLastPage, SIGNAL(triggered(bool)), SLOT(goLastPage()));
  connect(actionPrevPage, SIGNAL(triggered(bool)), SLOT(goPrevPage()));
  connect(actionNextPage, SIGNAL(triggered(bool)), SLOT(goNextPage()));
  connect(actionPrevPageQ, SIGNAL(triggered(bool)), this, SLOT(goPrevPage()));
  connect(actionNextPageW, SIGNAL(triggered(bool)), this, SLOT(goNextPage()));
  connect(actionPrevSelectedPage, SIGNAL(triggered(bool)), SLOT(goPrevSelectedPage()));
  connect(actionNextSelectedPage, SIGNAL(triggered(bool)), SLOT(goNextSelectedPage()));
  connect(actionPrevSelectedPageQ, SIGNAL(triggered(bool)), this, SLOT(goPrevSelectedPage()));
  connect(actionNextSelectedPageW, SIGNAL(triggered(bool)), this, SLOT(goNextSelectedPage()));
  connect(actionAbout, SIGNAL(triggered(bool)), this, SLOT(showAboutDialog()));
  connect(&OutOfMemoryHandler::instance(), SIGNAL(outOfMemory()), SLOT(handleOutOfMemorySituation()));
  connect(prevPageBtn, &QToolButton::clicked, this, [this]() {
    if (filterSelectedBtn->isChecked()) {
      goPrevSelectedPage();
    } else {
      goPrevPage();
    }
  });
  connect(nextPageBtn, &QToolButton::clicked, this, [this]() {
    if (filterSelectedBtn->isChecked()) {
      goNextSelectedPage();
    } else {
      goNextPage();
    }
  });

  connect(actionSwitchFilter1, SIGNAL(triggered(bool)), SLOT(switchFilter1()));
  connect(actionSwitchFilter2, SIGNAL(triggered(bool)), SLOT(switchFilter2()));
  connect(actionSwitchFilter3, SIGNAL(triggered(bool)), SLOT(switchFilter3()));
  connect(actionSwitchFilter4, SIGNAL(triggered(bool)), SLOT(switchFilter4()));
  connect(actionSwitchFilter5, SIGNAL(triggered(bool)), SLOT(switchFilter5()));
  connect(actionSwitchFilter6, SIGNAL(triggered(bool)), SLOT(switchFilter6()));

  connect(filterList->selectionModel(), SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)), this,
          SLOT(filterSelectionChanged(const QItemSelection&)));
  connect(filterList, SIGNAL(launchBatchProcessing()), this, SLOT(startBatchProcessing()));

  connect(m_workerThreadPool.get(), SIGNAL(taskResult(const BackgroundTaskPtr&, const FilterResultPtr&)), this,
          SLOT(filterResult(const BackgroundTaskPtr&, const FilterResultPtr&)));

  connect(m_thumbSequence.get(),
          SIGNAL(newSelectionLeader(const PageInfo&, const QRectF&, ThumbnailSequence::SelectionFlags)), this,
          SLOT(currentPageChanged(const PageInfo&, const QRectF&, ThumbnailSequence::SelectionFlags)));
  connect(m_thumbSequence.get(), SIGNAL(pageContextMenuRequested(const PageInfo&, const QPoint&, bool)), this,
          SLOT(pageContextMenuRequested(const PageInfo&, const QPoint&, bool)));
  connect(m_thumbSequence.get(), SIGNAL(pastLastPageContextMenuRequested(const QPoint&)),
          SLOT(pastLastPageContextMenuRequested(const QPoint&)));

  connect(thumbView->verticalScrollBar(), SIGNAL(sliderMoved(int)), this, SLOT(thumbViewScrolled()));
  connect(thumbView->verticalScrollBar(), SIGNAL(valueChanged(int)), this, SLOT(thumbViewScrolled()));
  connect(focusButton, SIGNAL(clicked(bool)), this, SLOT(thumbViewFocusToggled(bool)));
  connect(sortOptions, SIGNAL(currentIndexChanged(int)), this, SLOT(pageOrderingChanged(int)));

  connect(actionFixDpi, SIGNAL(triggered(bool)), SLOT(fixDpiDialogRequested()));
  connect(actionRelinking, SIGNAL(triggered(bool)), SLOT(showRelinkingDialog()));
#ifndef NDEBUG
  connect(actionDebug, SIGNAL(toggled(bool)), SLOT(debugToggled(bool)));
#else
  actionDebug->setVisible(false);
#endif

  connect(actionSettings, SIGNAL(triggered(bool)), this, SLOT(openSettingsDialog()));
  connect(actionDefaults, SIGNAL(triggered(bool)), this, SLOT(openDefaultParamsDialog()));

  connect(actionNewProject, SIGNAL(triggered(bool)), this, SLOT(newProject()));
  connect(actionOpenProject, SIGNAL(triggered(bool)), this, SLOT(openProject()));
  connect(actionSaveProject, SIGNAL(triggered(bool)), this, SLOT(saveProjectTriggered()));
  connect(actionSaveProjectAs, SIGNAL(triggered(bool)), this, SLOT(saveProjectAsTriggered()));
  connect(actionCloseProject, SIGNAL(triggered(bool)), this, SLOT(closeProject()));
  connect(actionQuit, SIGNAL(triggered(bool)), this, SLOT(close()));

  updateProjectActions();
  updateWindowTitle();
  updateMainArea();

  QSettings settings;
  if (settings.value("mainWindow/maximized") == false) {
    const QVariant geom(settings.value("mainWindow/nonMaximizedGeometry"));
    if (!restoreGeometry(geom.toByteArray())) {
      resize(1014, 689);  // A sensible value.
    }
  }
  m_autoSaveProject = settings.value("settings/auto_save_project").toBool();

  m_maxLogicalThumbSizeUpdater.setSingleShot(true);
  connect(&m_maxLogicalThumbSizeUpdater, &QTimer::timeout, this, &MainWindow::updateMaxLogicalThumbSize);

  m_sceneItemsPosUpdater.setSingleShot(true);
  connect(&m_sceneItemsPosUpdater, &QTimer::timeout, m_thumbSequence.get(), &ThumbnailSequence::updateSceneItemsPos);
}

MainWindow::~MainWindow() {
  m_interactiveQueue->cancelAndClear();
  if (m_batchQueue) {
    m_batchQueue->cancelAndClear();
  }
  m_workerThreadPool->shutdown();

  removeWidgetsFromLayout(m_imageFrameLayout);
  removeWidgetsFromLayout(m_optionsFrameLayout);
  m_tabbedDebugImages->clear();
}

PageSequence MainWindow::allPages() const {
  return m_thumbSequence->toPageSequence();
}

std::set<PageId> MainWindow::selectedPages() const {
  return m_thumbSequence->selectedItems();
}

std::vector<PageRange> MainWindow::selectedRanges() const {
  return m_thumbSequence->selectedRanges();
}

void MainWindow::switchToNewProject(const intrusive_ptr<ProjectPages>& pages,
                                    const QString& out_dir,
                                    const QString& project_file_path,
                                    const ProjectReader* project_reader) {
  stopBatchProcessing(CLEAR_MAIN_AREA);
  m_interactiveQueue->cancelAndClear();

  if (!out_dir.isEmpty()) {
    Utils::maybeCreateCacheDir(out_dir);
  }
  m_pages = pages;
  m_projectFile = project_file_path;

  if (project_reader) {
    m_selectedPage = project_reader->selectedPage();
  }

  intrusive_ptr<FileNameDisambiguator> disambiguator;
  if (project_reader) {
    disambiguator = project_reader->namingDisambiguator();
  } else {
    disambiguator.reset(new FileNameDisambiguator);
  }

  m_outFileNameGen = OutputFileNameGenerator(disambiguator, out_dir, pages->layoutDirection());
  // These two need to go in this order.
  updateDisambiguationRecords(pages->toPageSequence(IMAGE_VIEW));

  // Recreate the stages and load their state.
  m_stages = make_intrusive<StageSequence>(pages, newPageSelectionAccessor());
  if (project_reader) {
    project_reader->readFilterSettings(m_stages->filters());
  }

  // Connect the filter list model to the view and select
  // the first item.
  {
    ScopedIncDec<int> guard(m_ignoreSelectionChanges);
    filterList->setStages(m_stages);
    filterList->selectRow(0);
    m_curFilter = 0;
    // Setting a data model also implicitly sets a new
    // selection model, so we have to reconnect to it.
    connect(filterList->selectionModel(), SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)), this,
            SLOT(filterSelectionChanged(const QItemSelection&)));
  }

  updateSortOptions();

  m_contentBoxPropagator = std::make_unique<ContentBoxPropagator>(
      m_stages->pageLayoutFilter(), createCompositeCacheDrivenTask(m_stages->selectContentFilterIdx()));

  m_pageOrientationPropagator = std::make_unique<PageOrientationPropagator>(
      m_stages->pageSplitFilter(), createCompositeCacheDrivenTask(m_stages->fixOrientationFilterIdx()));

  // Thumbnails are stored relative to the output directory,
  // so recreate the thumbnail cache.
  if (out_dir.isEmpty()) {
    m_thumbnailCache.reset();
  } else {
    m_thumbnailCache = Utils::createThumbnailCache(m_outFileNameGen.outDir());
  }
  resetThumbSequence(currentPageOrderProvider());

  removeFilterOptionsWidget();
  updateProjectActions();
  updateWindowTitle();
  updateMainArea();

  if (!QDir(out_dir).exists()) {
    showRelinkingDialog();
  }
}  // MainWindow::switchToNewProject

void MainWindow::showNewOpenProjectPanel() {
  std::unique_ptr<QWidget> outer_widget(new QWidget);
  QGridLayout* layout = new QGridLayout(outer_widget.get());
  outer_widget->setLayout(layout);

  NewOpenProjectPanel* nop = new NewOpenProjectPanel(outer_widget.get());
  // We use asynchronous connections because otherwise we
  // would be deleting a widget from its event handler, which
  // Qt doesn't like.
  connect(nop, SIGNAL(newProject()), this, SLOT(newProject()), Qt::QueuedConnection);
  connect(nop, SIGNAL(openProject()), this, SLOT(openProject()), Qt::QueuedConnection);
  connect(nop, SIGNAL(openRecentProject(const QString&)), this, SLOT(openProject(const QString&)),
          Qt::QueuedConnection);

  layout->addWidget(nop, 1, 1);
  layout->setColumnStretch(0, 1);
  layout->setColumnStretch(2, 1);
  layout->setRowStretch(0, 1);
  layout->setRowStretch(2, 1);
  setImageWidget(outer_widget.release(), TRANSFER_OWNERSHIP);

  filterList->setBatchProcessingPossible(false);
}  // MainWindow::showNewOpenProjectPanel

void MainWindow::createBatchProcessingWidget() {
  m_batchProcessingWidget.reset(new QWidget);
  QGridLayout* layout = new QGridLayout(m_batchProcessingWidget.get());
  m_batchProcessingWidget->setLayout(layout);

  SkinnedButton* stop_btn = new SkinnedButton(":/icons/stop-big.png", ":/icons/stop-big-hovered.png",
                                              ":/icons/stop-big-pressed.png", m_batchProcessingWidget.get());
  stop_btn->setStatusTip(tr("Stop batch processing"));

  class LowerPanel : public QWidget {
   public:
    LowerPanel(QWidget* parent = 0) : QWidget(parent) { ui.setupUi(this); }

    Ui::BatchProcessingLowerPanel ui;
  };


  LowerPanel* lower_panel = new LowerPanel(m_batchProcessingWidget.get());
  m_checkBeepWhenFinished = [lower_panel]() { return lower_panel->ui.beepWhenFinished->isChecked(); };

  int row = 0;  // Row 0 is reserved.
  layout->addWidget(stop_btn, ++row, 1, Qt::AlignCenter);
  layout->addWidget(lower_panel, ++row, 0, 1, 3, Qt::AlignHCenter | Qt::AlignTop);
  layout->setColumnStretch(0, 1);
  layout->setColumnStretch(2, 1);
  layout->setRowStretch(0, 1);
  layout->setRowStretch(row, 1);

  connect(stop_btn, SIGNAL(clicked()), SLOT(stopBatchProcessing()));
}  // MainWindow::createBatchProcessingWidget

void MainWindow::updateThumbViewMinWidth() {
  const int sb = thumbView->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
  int inner_width = thumbView->maximumViewportSize().width() - sb;
  if (thumbView->style()->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents, 0, thumbView)) {
    inner_width -= thumbView->frameWidth() * 2;
  }
  const int delta_x = thumbView->size().width() - inner_width;
  thumbView->setMinimumWidth((int) std::ceil(m_maxLogicalThumbSize.width() + delta_x));
}

void MainWindow::setupThumbView() {
  updateThumbViewMinWidth();
  m_thumbSequence->attachView(thumbView);
  thumbView->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
  if ((obj == thumbView) && (ev->type() == QEvent::Resize)) {
    if (!m_sceneItemsPosUpdater.isActive()) {
      m_sceneItemsPosUpdater.start(150);
    }
  }

  if ((obj == thumbView || obj == thumbView->verticalScrollBar()) && (ev->type() == QEvent::Wheel)) {
    QWheelEvent* wheel_event = static_cast<QWheelEvent*>(ev);
    if (wheel_event->modifiers() == Qt::AltModifier) {
      scaleThumbnails(wheel_event);
      wheel_event->accept();
      return true;
    }
  }
  return false;
}

void MainWindow::closeEvent(QCloseEvent* const event) {
  if (m_closing) {
    event->accept();
  } else {
    event->ignore();
    startTimer(0);
  }
}

void MainWindow::timerEvent(QTimerEvent* const event) {
  // We only use the timer event for delayed closing of the window.
  killTimer(event->timerId());

  if (closeProjectInteractive()) {
    m_closing = true;
    QSettings settings;
    settings.setValue("mainWindow/maximized", isMaximized());
    if (!isMaximized()) {
      settings.setValue("mainWindow/nonMaximizedGeometry", saveGeometry());
    }
    close();
  }
}

MainWindow::SavePromptResult MainWindow::promptProjectSave() {
  QMessageBox msgBox(QMessageBox::Question, tr("Save Project"), tr("Save the project?"),
                     QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
  msgBox.setDefaultButton(QMessageBox::Yes);

  switch (msgBox.exec()) {
    case QMessageBox::Yes:
      return SAVE;
    case QMessageBox::No:
      return DONT_SAVE;
    default:
      return CANCEL;
  }
}

bool MainWindow::compareFiles(const QString& fpath1, const QString& fpath2) {
  QFile file1(fpath1);
  QFile file2(fpath2);

  if (!file1.open(QIODevice::ReadOnly)) {
    return false;
  }
  if (!file2.open(QIODevice::ReadOnly)) {
    return false;
  }

  if (!file1.isSequential() && !file2.isSequential()) {
    if (file1.size() != file2.size()) {
      return false;
    }
  }

  const int chunk_size = 4096;
  while (true) {
    const QByteArray chunk1(file1.read(chunk_size));
    const QByteArray chunk2(file2.read(chunk_size));
    if (chunk1.size() != chunk2.size()) {
      return false;
    } else if (chunk1.size() == 0) {
      return true;
    }
  }
}

intrusive_ptr<const PageOrderProvider> MainWindow::currentPageOrderProvider() const {
  const int idx = sortOptions->currentIndex();
  if (idx < 0) {
    return nullptr;
  }

  const intrusive_ptr<AbstractFilter> filter(m_stages->filterAt(m_curFilter));

  return filter->pageOrderOptions()[idx].provider();
}

void MainWindow::updateSortOptions() {
  const ScopedIncDec<int> guard(m_ignorePageOrderingChanges);

  const intrusive_ptr<AbstractFilter> filter(m_stages->filterAt(m_curFilter));

  sortOptions->clear();

  for (const PageOrderOption& opt : filter->pageOrderOptions()) {
    sortOptions->addItem(opt.name());
  }

  sortOptions->setVisible(sortOptions->count() > 0);

  if (sortOptions->count() > 0) {
    sortOptions->setCurrentIndex(filter->selectedPageOrder());
  }
}

void MainWindow::resetThumbSequence(const intrusive_ptr<const PageOrderProvider>& page_order_provider,
                                    const ThumbnailSequence::SelectionAction selection_action) {
  if (m_thumbnailCache) {
    const intrusive_ptr<CompositeCacheDrivenTask> task(createCompositeCacheDrivenTask(m_curFilter));

    m_thumbSequence->setThumbnailFactory(
        make_intrusive<ThumbnailFactory>(m_thumbnailCache, m_maxLogicalThumbSize, task));
  }

  m_thumbSequence->reset(m_pages->toPageSequence(getCurrentView()), selection_action, page_order_provider);

  if (!m_thumbnailCache) {
    // Empty project.
    assert(m_pages->numImages() == 0);
    m_thumbSequence->setThumbnailFactory(nullptr);
  }

  if (selection_action != ThumbnailSequence::KEEP_SELECTION) {
    const PageId page(m_selectedPage.get(getCurrentView()));
    if (m_thumbSequence->setSelection(page)) {
      // OK
    } else if (m_thumbSequence->setSelection(PageId(page.imageId(), PageId::LEFT_PAGE))) {
      // OK
    } else if (m_thumbSequence->setSelection(PageId(page.imageId(), PageId::RIGHT_PAGE))) {
      // OK
    } else if (m_thumbSequence->setSelection(PageId(page.imageId(), PageId::SINGLE_PAGE))) {
      // OK
    } else {
      // Last resort.
      m_thumbSequence->setSelection(m_thumbSequence->firstPage().id());
    }
  }
}

void MainWindow::setOptionsWidget(FilterOptionsWidget* widget, const Ownership ownership) {
  if (isBatchProcessingInProgress()) {
    if (ownership == TRANSFER_OWNERSHIP) {
      delete widget;
    }

    return;
  }

  if (m_optionsWidget != widget) {
    removeWidgetsFromLayout(m_optionsFrameLayout);
  }
  // Delete the old widget we were owning, if any.
  m_optionsWidgetCleanup.clear();

  if (ownership == TRANSFER_OWNERSHIP) {
    m_optionsWidgetCleanup.add(widget);
  }

  if (m_optionsWidget == widget) {
    return;
  }

  if (m_optionsWidget) {
    disconnect(m_optionsWidget, SIGNAL(reloadRequested()), this, SLOT(reloadRequested()));
    disconnect(m_optionsWidget, SIGNAL(invalidateThumbnail(const PageId&)), this,
               SLOT(invalidateThumbnail(const PageId&)));
    disconnect(m_optionsWidget, SIGNAL(invalidateThumbnail(const PageInfo&)), this,
               SLOT(invalidateThumbnail(const PageInfo&)));
    disconnect(m_optionsWidget, SIGNAL(invalidateAllThumbnails()), this, SLOT(invalidateAllThumbnails()));
    disconnect(m_optionsWidget, SIGNAL(goToPage(const PageId&)), this, SLOT(goToPage(const PageId&)));
  }

  m_optionsFrameLayout->addWidget(widget);
  m_optionsWidget = widget;

  // We use an asynchronous connection here, because the slot
  // will probably delete the options panel, which could be
  // responsible for the emission of this signal.  Qt doesn't
  // like when we delete an object while it's emitting a singal.
  connect(widget, SIGNAL(reloadRequested()), this, SLOT(reloadRequested()), Qt::QueuedConnection);
  connect(widget, SIGNAL(invalidateThumbnail(const PageId&)), this, SLOT(invalidateThumbnail(const PageId&)));
  connect(widget, SIGNAL(invalidateThumbnail(const PageInfo&)), this, SLOT(invalidateThumbnail(const PageInfo&)));
  connect(widget, SIGNAL(invalidateAllThumbnails()), this, SLOT(invalidateAllThumbnails()));
  connect(widget, SIGNAL(goToPage(const PageId&)), this, SLOT(goToPage(const PageId&)));
}  // MainWindow::setOptionsWidget

void MainWindow::setImageWidget(QWidget* widget, const Ownership ownership, DebugImages* debug_images, bool overlay) {
  if (isBatchProcessingInProgress() && (widget != m_batchProcessingWidget.get())) {
    if (ownership == TRANSFER_OWNERSHIP) {
      delete widget;
    }
    return;
  }

  if (!overlay) {
    removeImageWidget();
  }

  if (ownership == TRANSFER_OWNERSHIP) {
    m_imageWidgetCleanup.add(widget);
  }

  if (!debug_images || debug_images->empty()) {
    if (widget != m_imageFrameLayout->currentWidget()) {
      m_imageFrameLayout->addWidget(widget);
      if (overlay) {
        m_imageFrameLayout->setCurrentWidget(widget);
      }
    }
  } else {
    m_tabbedDebugImages->addTab(widget, "Main");
    AutoRemovingFile file;
    QString label;
    while (!(file = debug_images->retrieveNext(&label)).get().isNull()) {
      QWidget* view = new DebugImageView(file);
      m_imageWidgetCleanup.add(view);
      m_tabbedDebugImages->addTab(view, label);
    }
    m_imageFrameLayout->addWidget(m_tabbedDebugImages.get());
  }
}  // MainWindow::setImageWidget

void MainWindow::removeImageWidget() {
  removeWidgetsFromLayout(m_imageFrameLayout);

  m_tabbedDebugImages->clear();
  // Delete the old widget we were owning, if any.
  m_imageWidgetCleanup.clear();
}

void MainWindow::invalidateThumbnail(const PageId& page_id) {
  m_thumbSequence->invalidateThumbnail(page_id);
}

void MainWindow::invalidateThumbnail(const PageInfo& page_info) {
  m_thumbSequence->invalidateThumbnail(page_info);
}

void MainWindow::invalidateAllThumbnails() {
  m_thumbSequence->invalidateAllThumbnails();
}

intrusive_ptr<AbstractCommand<void>> MainWindow::relinkingDialogRequester() {
  class Requester : public AbstractCommand<void> {
   public:
    Requester(MainWindow* wnd) : m_wnd(wnd) {}

    virtual void operator()() {
      if (!m_wnd.isNull()) {
        m_wnd->showRelinkingDialog();
      }
    }

   private:
    QPointer<MainWindow> m_wnd;
  };


  return make_intrusive<Requester>(this);
}

void MainWindow::showRelinkingDialog() {
  if (!isProjectLoaded()) {
    return;
  }

  RelinkingDialog* dialog = new RelinkingDialog(m_projectFile, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::WindowModal);

  m_pages->listRelinkablePaths(dialog->pathCollector());
  dialog->pathCollector()(RelinkablePath(m_outFileNameGen.outDir(), RelinkablePath::Dir));

  connect(dialog, &QDialog::accepted, [this, dialog]() { this->performRelinking(dialog->relinker()); });

  dialog->show();
}

void MainWindow::performRelinking(const intrusive_ptr<AbstractRelinker>& relinker) {
  assert(relinker);

  if (!isProjectLoaded()) {
    return;
  }

  m_pages->performRelinking(*relinker);
  m_stages->performRelinking(*relinker);
  m_outFileNameGen.performRelinking(*relinker);

  Utils::maybeCreateCacheDir(m_outFileNameGen.outDir());

  m_thumbnailCache->setThumbDir(Utils::outputDirToThumbDir(m_outFileNameGen.outDir()));
  resetThumbSequence(currentPageOrderProvider());
  m_selectedPage.set(m_thumbSequence->selectionLeader().id(), getCurrentView());

  reloadRequested();
}

void MainWindow::goFirstPage() {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }

  const PageInfo first_page(m_thumbSequence->firstPage());
  if (!first_page.isNull()) {
    goToPage(first_page.id());
  }
}

void MainWindow::goLastPage() {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }

  const PageInfo last_page(m_thumbSequence->lastPage());
  if (!last_page.isNull()) {
    goToPage(last_page.id());
  }
}

void MainWindow::goNextPage() {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }

  const PageInfo next_page(m_thumbSequence->nextPage(m_thumbSequence->selectionLeader().id()));
  if (!next_page.isNull()) {
    goToPage(next_page.id());
  }
}

void MainWindow::goPrevPage() {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }

  const PageInfo prev_page(m_thumbSequence->prevPage(m_thumbSequence->selectionLeader().id()));
  if (!prev_page.isNull()) {
    goToPage(prev_page.id());
  }
}

void MainWindow::goNextSelectedPage() {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }

  const PageInfo next_selected_page(m_thumbSequence->nextSelectedPage(m_thumbSequence->selectionLeader().id()));
  if (!next_selected_page.isNull()) {
    goToPage(next_selected_page.id(), ThumbnailSequence::KEEP_SELECTION);
  }
}

void MainWindow::goPrevSelectedPage() {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }

  const PageInfo prev_selected_page(m_thumbSequence->prevSelectedPage(m_thumbSequence->selectionLeader().id()));
  if (!prev_selected_page.isNull()) {
    goToPage(prev_selected_page.id(), ThumbnailSequence::KEEP_SELECTION);
  }
}

void MainWindow::goToPage(const PageId& page_id, const ThumbnailSequence::SelectionAction selection_action) {
  focusButton->setChecked(true);

  m_thumbSequence->setSelection(page_id, selection_action);

  // If the page was already selected, it will be reloaded.
  // That's by design.
  updateMainArea();

  if (m_autoSaveTimer.remainingTime() <= 0) {
    m_autoSaveTimer.start(30000);
  }
}

void MainWindow::currentPageChanged(const PageInfo& page_info,
                                    const QRectF& thumb_rect,
                                    const ThumbnailSequence::SelectionFlags flags) {
  m_selectedPage.set(page_info.id(), getCurrentView());

  if ((flags & ThumbnailSequence::SELECTED_BY_USER) || focusButton->isChecked()) {
    if (!(flags & ThumbnailSequence::AVOID_SCROLLING_TO)) {
      thumbView->ensureVisible(thumb_rect, 0, 0);
    }
  }

  if (flags & ThumbnailSequence::SELECTED_BY_USER) {
    if (isBatchProcessingInProgress()) {
      stopBatchProcessing();
    } else if (!(flags & ThumbnailSequence::REDUNDANT_SELECTION)) {
      // Start loading / processing the newly selected page.
      updateMainArea();
    }
  }

  if (flags & ThumbnailSequence::SELECTED_BY_USER) {
    if (m_autoSaveTimer.remainingTime() <= 0) {
      m_autoSaveTimer.start(30000);
    }
  }
}

void MainWindow::autoSaveProject() {
  if (m_projectFile.isEmpty()) {
    return;
  }

  if (!m_autoSaveProject) {
    return;
  }

  saveProjectWithFeedback(m_projectFile);
}

void MainWindow::pageContextMenuRequested(const PageInfo& page_info_, const QPoint& screen_pos, bool selected) {
  if (isBatchProcessingInProgress()) {
    return;
  }
  // Make a copy to prevent it from being invalidated.
  const PageInfo page_info(page_info_);

  if (!selected) {
    goToPage(page_info.id());
  }

  QMenu menu;

  QAction* ins_before = menu.addAction(QIcon(":/icons/insert-before-16.png"), tr("Insert before ..."));
  QAction* ins_after = menu.addAction(QIcon(":/icons/insert-after-16.png"), tr("Insert after ..."));

  menu.addSeparator();

  QAction* remove = menu.addAction(QIcon(":/icons/user-trash.png"), tr("Remove from project ..."));

  QAction* action = menu.exec(screen_pos);
  if (action == ins_before) {
    showInsertFileDialog(BEFORE, page_info.imageId());
  } else if (action == ins_after) {
    showInsertFileDialog(AFTER, page_info.imageId());
  } else if (action == remove) {
    showRemovePagesDialog(m_thumbSequence->selectedItems());
  }
}  // MainWindow::pageContextMenuRequested

void MainWindow::pastLastPageContextMenuRequested(const QPoint& screen_pos) {
  if (!isProjectLoaded()) {
    return;
  }

  QMenu menu;
  menu.addAction(QIcon(":/icons/insert-here-16.png"), tr("Insert here ..."));

  if (menu.exec(screen_pos)) {
    showInsertFileDialog(BEFORE, ImageId());
  }
}

void MainWindow::thumbViewFocusToggled(const bool checked) {
  const QRectF rect(m_thumbSequence->selectionLeaderSceneRect());
  if (rect.isNull()) {
    // No selected items.
    return;
  }

  if (checked) {
    thumbView->ensureVisible(rect, 0, 0);
  }
}

void MainWindow::thumbViewScrolled() {
  const QRectF rect(m_thumbSequence->selectionLeaderSceneRect());
  if (rect.isNull()) {
    // No items selected.
    return;
  }

  const QRectF viewport_rect(thumbView->viewport()->rect());
  const QRectF viewport_item_rect(thumbView->viewportTransform().mapRect(rect));

  const double intersection_threshold = 0.5;
  if ((viewport_item_rect.top() >= viewport_rect.top())
      && (viewport_item_rect.top() + viewport_item_rect.height() * intersection_threshold <= viewport_rect.bottom())) {
    // Item is visible.
  } else if ((viewport_item_rect.bottom() <= viewport_rect.bottom())
             && (viewport_item_rect.bottom() - viewport_item_rect.height() * intersection_threshold
                 >= viewport_rect.top())) {
    // Item is visible.
  } else {
    focusButton->setChecked(false);
  }
}

void MainWindow::filterSelectionChanged(const QItemSelection& selected) {
  if (m_ignoreSelectionChanges) {
    return;
  }

  if (selected.empty()) {
    return;
  }

  m_interactiveQueue->cancelAndClear();
  if (m_batchQueue) {
    // Should not happen, but just in case.
    m_batchQueue->cancelAndClear();
  }

  const bool was_below_fix_orientation = isBelowFixOrientation(m_curFilter);
  const bool was_below_select_content = isBelowSelectContent(m_curFilter);
  m_curFilter = selected.front().top();
  const bool now_below_fix_orientation = isBelowFixOrientation(m_curFilter);
  const bool now_below_select_content = isBelowSelectContent(m_curFilter);

  m_stages->filterAt(m_curFilter)->selected();

  updateSortOptions();

  // Propagate context boxes down the stage list, if necessary.
  if (!was_below_select_content && now_below_select_content) {
    // IMPORTANT: this needs to go before resetting thumbnails,
    // because it may affect them.
    if (m_contentBoxPropagator) {
      m_contentBoxPropagator->propagate(*m_pages);
    }  // Otherwise probably no project is loaded.
  }
  // Propagate page orientations (that might have changed) to the "Split Pages" stage.
  if (!was_below_fix_orientation && now_below_fix_orientation) {
    // IMPORTANT: this needs to go before resetting thumbnails,
    // because it may affect them.
    if (m_pageOrientationPropagator) {
      m_pageOrientationPropagator->propagate(*m_pages);
    }  // Otherwise probably no project is loaded.
  }

  const int hor_scroll_bar_pos = thumbView->horizontalScrollBar()->value();
  const int ver_scroll_bar_pos = thumbView->verticalScrollBar()->value();

  resetThumbSequence(currentPageOrderProvider(), ThumbnailSequence::KEEP_SELECTION);

  if (!focusButton->isChecked()) {
    thumbView->horizontalScrollBar()->setValue(hor_scroll_bar_pos);
    thumbView->verticalScrollBar()->setValue(ver_scroll_bar_pos);
  }

  // load default settings for all the pages
  for (const PageInfo& pageInfo : m_thumbSequence->toPageSequence()) {
    for (int i = 0; i < m_stages->count(); i++) {
      m_stages->filterAt(i)->loadDefaultSettings(pageInfo);
    }
  }

  updateMainArea();
}  // MainWindow::filterSelectionChanged

void MainWindow::switchFilter1() {
  filterList->selectRow(0);
}

void MainWindow::switchFilter2() {
  filterList->selectRow(1);
}

void MainWindow::switchFilter3() {
  filterList->selectRow(2);
}

void MainWindow::switchFilter4() {
  filterList->selectRow(3);
}

void MainWindow::switchFilter5() {
  filterList->selectRow(4);
}

void MainWindow::switchFilter6() {
  filterList->selectRow(5);
}

void MainWindow::pageOrderingChanged(int idx) {
  if (m_ignorePageOrderingChanges) {
    return;
  }

  const int hor_scroll_bar_pos = thumbView->horizontalScrollBar()->value();
  const int ver_scroll_bar_pos = thumbView->verticalScrollBar()->value();

  m_stages->filterAt(m_curFilter)->selectPageOrder(idx);

  m_thumbSequence->reset(m_pages->toPageSequence(getCurrentView()), ThumbnailSequence::KEEP_SELECTION,
                         currentPageOrderProvider());

  if (!focusButton->isChecked()) {
    thumbView->horizontalScrollBar()->setValue(hor_scroll_bar_pos);
    thumbView->verticalScrollBar()->setValue(ver_scroll_bar_pos);
  }
}

void MainWindow::reloadRequested() {
  // Start loading / processing the current page.
  updateMainArea();
}

void MainWindow::startBatchProcessing() {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }

  m_interactiveQueue->cancelAndClear();

  m_batchQueue.reset(new ProcessingTaskQueue);
  PageInfo page(m_thumbSequence->selectionLeader());
  for (; !page.isNull(); page = m_thumbSequence->nextPage(page.id())) {
    for (int i = 0; i < m_stages->count(); i++) {
      m_stages->filterAt(i)->loadDefaultSettings(page);
    }
    m_batchQueue->addProcessingTask(page, createCompositeTask(page, m_curFilter, /*batch=*/true, m_debug));
  }

  focusButton->setChecked(true);

  removeFilterOptionsWidget();
  filterList->setBatchProcessingInProgress(true);
  filterList->setEnabled(false);

  BackgroundTaskPtr task(m_batchQueue->takeForProcessing());
  if (task) {
    do {
      m_workerThreadPool->submitTask(task);
      if (!m_workerThreadPool->hasSpareCapacity()) {
        break;
      }
    } while ((task = m_batchQueue->takeForProcessing()));
  } else {
    stopBatchProcessing();
  }

  page = m_batchQueue->selectedPage();
  if (!page.isNull()) {
    m_thumbSequence->setSelection(page.id());
  }
  // Display the batch processing screen.
  updateMainArea();
}  // MainWindow::startBatchProcessing

void MainWindow::stopBatchProcessing(MainAreaAction main_area) {
  if (!isBatchProcessingInProgress()) {
    return;
  }

  const PageInfo page(m_batchQueue->selectedPage());
  if (!page.isNull()) {
    m_thumbSequence->setSelection(page.id());
  }

  m_batchQueue->cancelAndClear();
  m_batchQueue.reset();

  filterList->setBatchProcessingInProgress(false);
  filterList->setEnabled(true);

  switch (main_area) {
    case UPDATE_MAIN_AREA:
      updateMainArea();
      break;
    case CLEAR_MAIN_AREA:
      removeImageWidget();
      break;
  }

  resetThumbSequence(currentPageOrderProvider());
}

void MainWindow::filterResult(const BackgroundTaskPtr& task, const FilterResultPtr& result) {
  // Cancelled or not, we must mark it as finished.
  m_interactiveQueue->processingFinished(task);
  if (m_batchQueue) {
    m_batchQueue->processingFinished(task);
  }

  if (task->isCancelled()) {
    return;
  }

  if (!isBatchProcessingInProgress()) {
    if (!result->filter()) {
      // Error loading file.  No special action is necessary.
    } else if (result->filter() != m_stages->filterAt(m_curFilter)) {
      // Error from one of the previous filters.
      const int idx = m_stages->findFilter(result->filter());
      assert(idx >= 0);
      m_curFilter = idx;

      ScopedIncDec<int> selection_guard(m_ignoreSelectionChanges);
      filterList->selectRow(idx);
    }
  }

  // This needs to be done even if batch processing is taking place,
  // for instance because thumbnail invalidation is done from here.
  result->updateUI(this);

  if (isBatchProcessingInProgress()) {
    if (m_batchQueue->allProcessed()) {
      stopBatchProcessing();

      QApplication::alert(this);  // Flash the taskbar entry.
      if (m_checkBeepWhenFinished()) {
#if defined(Q_OS_UNIX)
        QString ext_play_cmd("play /usr/share/sounds/freedesktop/stereo/bell.oga");
#else
        QString ext_play_cmd;
#endif
        QSettings settings;
        QString cmd = settings.value("main_window/external_alarm_cmd", ext_play_cmd).toString();
        if (cmd.isEmpty()) {
          QApplication::beep();
        } else {
          Q_UNUSED(std::system(cmd.toStdString().c_str()));
        }
      }

      if (m_selectedPage.get(getCurrentView()) == m_thumbSequence->lastPage().id()) {
        // If batch processing finished at the last page, jump to the first one.
        goFirstPage();
      }

      return;
    }

    do {
      const BackgroundTaskPtr task(m_batchQueue->takeForProcessing());
      if (!task) {
        break;
      }
      m_workerThreadPool->submitTask(task);
    } while (m_workerThreadPool->hasSpareCapacity());

    const PageInfo page(m_batchQueue->selectedPage());
    if (!page.isNull()) {
      m_thumbSequence->setSelection(page.id());
    }
  }
}  // MainWindow::filterResult

void MainWindow::debugToggled(const bool enabled) {
  m_debug = enabled;
}

void MainWindow::fixDpiDialogRequested() {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }

  assert(!m_fixDpiDialog);
  m_fixDpiDialog = new FixDpiDialog(m_pages->toImageFileInfo(), this);
  m_fixDpiDialog->setAttribute(Qt::WA_DeleteOnClose);
  m_fixDpiDialog->setWindowModality(Qt::WindowModal);

  connect(m_fixDpiDialog, SIGNAL(accepted()), SLOT(fixedDpiSubmitted()));

  m_fixDpiDialog->show();
}

void MainWindow::fixedDpiSubmitted() {
  assert(m_fixDpiDialog);
  assert(m_pages);
  assert(m_thumbSequence);

  const PageInfo selected_page_before(m_thumbSequence->selectionLeader());

  m_pages->updateMetadataFrom(m_fixDpiDialog->files());

  // The thumbnail list also stores page metadata, including the DPI.
  m_thumbSequence->reset(m_pages->toPageSequence(getCurrentView()), ThumbnailSequence::KEEP_SELECTION,
                         m_thumbSequence->pageOrderProvider());

  const PageInfo selected_page_after(m_thumbSequence->selectionLeader());

  // Reload if the current page was affected.
  // Note that imageId() isn't supposed to change - we check just in case.
  if ((selected_page_before.imageId() != selected_page_after.imageId())
      || (selected_page_before.metadata() != selected_page_after.metadata())) {
    reloadRequested();
  }
}

void MainWindow::saveProjectTriggered() {
  if (m_projectFile.isEmpty()) {
    saveProjectAsTriggered();

    return;
  }

  if (saveProjectWithFeedback(m_projectFile)) {
    updateWindowTitle();
  }
}

void MainWindow::saveProjectAsTriggered() {
  // XXX: this function is duplicated in OutOfMemoryDialog.

  QString project_dir;
  if (!m_projectFile.isEmpty()) {
    project_dir = QFileInfo(m_projectFile).absolutePath();
  } else {
    QSettings settings;
    project_dir = settings.value("project/lastDir").toString();
  }

  QString project_file(
      QFileDialog::getSaveFileName(this, QString(), project_dir, tr("Scan Tailor Projects") + " (*.ScanTailor)"));
  if (project_file.isEmpty()) {
    return;
  }

  if (!project_file.endsWith(".ScanTailor", Qt::CaseInsensitive)) {
    project_file += ".ScanTailor";
  }

  if (saveProjectWithFeedback(project_file)) {
    m_projectFile = project_file;
    updateWindowTitle();

    QSettings settings;
    settings.setValue("project/lastDir", QFileInfo(m_projectFile).absolutePath());

    RecentProjects rp;
    rp.read();
    rp.setMostRecent(m_projectFile);
    rp.write();
  }
}  // MainWindow::saveProjectAsTriggered

void MainWindow::newProject() {
  if (!closeProjectInteractive()) {
    return;
  }

  // It will delete itself when it's done.
  ProjectCreationContext* context = new ProjectCreationContext(this);
  connect(context, SIGNAL(done(ProjectCreationContext*)), this, SLOT(newProjectCreated(ProjectCreationContext*)));
}

void MainWindow::newProjectCreated(ProjectCreationContext* context) {
  auto pages = make_intrusive<ProjectPages>(context->files(), ProjectPages::AUTO_PAGES, context->layoutDirection());
  switchToNewProject(pages, context->outDir());
}

void MainWindow::openProject() {
  if (!closeProjectInteractive()) {
    return;
  }

  QSettings settings;
  const QString project_dir(settings.value("project/lastDir").toString());

  const QString project_file(QFileDialog::getOpenFileName(this, tr("Open Project"), project_dir,
                                                          tr("Scan Tailor Projects") + " (*.ScanTailor)"));
  if (project_file.isEmpty()) {
    // Cancelled by user.
    return;
  }

  openProject(project_file);
}

void MainWindow::openProject(const QString& project_file) {
  QFile file(project_file);
  if (!file.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, tr("Error"), tr("Unable to open the project file."));

    return;
  }

  QDomDocument doc;
  if (!doc.setContent(&file)) {
    QMessageBox::warning(this, tr("Error"), tr("The project file is broken."));

    return;
  }

  file.close();

  ProjectOpeningContext* context = new ProjectOpeningContext(this, project_file, doc);
  connect(context, SIGNAL(done(ProjectOpeningContext*)), SLOT(projectOpened(ProjectOpeningContext*)));
  context->proceed();
}

void MainWindow::projectOpened(ProjectOpeningContext* context) {
  RecentProjects rp;
  rp.read();
  rp.setMostRecent(context->projectFile());
  rp.write();

  QSettings settings;
  settings.setValue("project/lastDir", QFileInfo(context->projectFile()).absolutePath());

  switchToNewProject(context->projectReader()->pages(), context->projectReader()->outputDirectory(),
                     context->projectFile(), context->projectReader());
}

void MainWindow::closeProject() {
  closeProjectInteractive();
}

void MainWindow::openSettingsDialog() {
  SettingsDialog* dialog = new SettingsDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::WindowModal);
  connect(dialog, SIGNAL(settingsChanged()), this, SLOT(onSettingsChanged()));
  dialog->show();
}

void MainWindow::openDefaultParamsDialog() {
  DefaultParamsDialog* dialog = new DefaultParamsDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::WindowModal);
  dialog->show();
}

void MainWindow::onSettingsChanged() {
  QSettings settings;
  bool need_invalidate = true;

  m_autoSaveProject = settings.value("settings/auto_save_project").toBool();

  if (auto* app = dynamic_cast<Application*>(qApp)) {
    app->installLanguage(settings.value("settings/language").toString());
  }

  if (m_thumbnailCache) {
    const QSize max_thumb_size = settings.value("settings/thumbnail_quality").toSize();
    if (m_thumbnailCache->getMaxThumbSize() != max_thumb_size) {
      m_thumbnailCache->setMaxThumbSize(max_thumb_size);
      need_invalidate = true;
    }
  }

  const QSizeF max_logical_thumb_size = settings.value("settings/max_logical_thumb_size").toSizeF();
  if (m_maxLogicalThumbSize != max_logical_thumb_size) {
    m_maxLogicalThumbSize = max_logical_thumb_size;
    updateMaxLogicalThumbSize();
    need_invalidate = false;
  }

  if (need_invalidate) {
    m_thumbSequence->invalidateAllThumbnails();
  }
}

void MainWindow::showAboutDialog() {
  Ui::AboutDialog ui;
  QDialog* dialog = new QDialog(this);
  ui.setupUi(dialog);
  ui.version->setText(QString(tr("version ")) + QString::fromUtf8(VERSION));

  QResource license(":/GPLv3.html");
  ui.licenseViewer->setHtml(QString::fromUtf8((const char*) license.data(), static_cast<int>(license.size())));

  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::WindowModal);
  dialog->show();
}

/**
 * This function is called asynchronously, always from the main thread.
 */
void MainWindow::handleOutOfMemorySituation() {
  deleteLater();

  m_outOfMemoryDialog->setParams(m_projectFile, m_stages, m_pages, m_selectedPage, m_outFileNameGen);

  closeProjectWithoutSaving();

  m_outOfMemoryDialog->setAttribute(Qt::WA_DeleteOnClose);
  m_outOfMemoryDialog.release()->show();
}

/**
 * Note: the removed widgets are not deleted.
 */
void MainWindow::removeWidgetsFromLayout(QLayout* layout) {
  QLayoutItem* child;
  while ((child = layout->takeAt(0))) {
    delete child;
  }
}

void MainWindow::removeFilterOptionsWidget() {
  removeWidgetsFromLayout(m_optionsFrameLayout);
  // Delete the old widget we were owning, if any.
  m_optionsWidgetCleanup.clear();

  m_optionsWidget = nullptr;
}

void MainWindow::updateProjectActions() {
  const bool loaded = isProjectLoaded();
  actionSaveProject->setEnabled(loaded);
  actionSaveProjectAs->setEnabled(loaded);
  actionFixDpi->setEnabled(loaded);
  actionRelinking->setEnabled(loaded);
}

bool MainWindow::isBatchProcessingInProgress() const {
  return m_batchQueue.get() != 0;
}

bool MainWindow::isProjectLoaded() const {
  return !m_outFileNameGen.outDir().isEmpty();
}

bool MainWindow::isBelowSelectContent() const {
  return isBelowSelectContent(m_curFilter);
}

bool MainWindow::isBelowSelectContent(const int filter_idx) const {
  return filter_idx > m_stages->selectContentFilterIdx();
}

bool MainWindow::isBelowFixOrientation(int filter_idx) const {
  return filter_idx > m_stages->fixOrientationFilterIdx();
}

bool MainWindow::isOutputFilter() const {
  return isOutputFilter(m_curFilter);
}

bool MainWindow::isOutputFilter(const int filter_idx) const {
  return filter_idx == m_stages->outputFilterIdx();
}

PageView MainWindow::getCurrentView() const {
  return m_stages->filterAt(m_curFilter)->getView();
}

void MainWindow::updateMainArea() {
  if (m_pages->numImages() == 0) {
    filterList->setBatchProcessingPossible(false);
    setDockWidgetsVisible(false);
    showNewOpenProjectPanel();
    m_statusBarPanel->clear();
  } else if (isBatchProcessingInProgress()) {
    filterList->setBatchProcessingPossible(false);
    setImageWidget(m_batchProcessingWidget.get(), KEEP_OWNERSHIP);
  } else {
    setDockWidgetsVisible(true);
    const PageInfo page(m_thumbSequence->selectionLeader());
    if (page.isNull()) {
      filterList->setBatchProcessingPossible(false);
      removeImageWidget();
      removeFilterOptionsWidget();
    } else {
      // Note that loadPageInteractive may reset it to false.
      filterList->setBatchProcessingPossible(true);
      PageSequence pageSequence = m_thumbSequence->toPageSequence();
      if (pageSequence.numPages() > 0) {
        m_statusBarPanel->updatePage(pageSequence.pageNo(page.id()) + 1, pageSequence.numPages(), page.id());
      }
      loadPageInteractive(page);
    }
  }
}

bool MainWindow::checkReadyForOutput(const PageId* ignore) const {
  return m_stages->pageLayoutFilter()->checkReadyForOutput(*m_pages, ignore);
}

void MainWindow::loadPageInteractive(const PageInfo& page) {
  assert(!isBatchProcessingInProgress());

  m_interactiveQueue->cancelAndClear();

  if (isOutputFilter() && !checkReadyForOutput(&page.id())) {
    filterList->setBatchProcessingPossible(false);

    const QString err_text(
        tr("Output is not yet possible, as the final size"
           " of pages is not yet known.\nTo determine it,"
           " run batch processing at \"Select Content\" or"
           " \"Margins\"."));

    removeFilterOptionsWidget();
    setImageWidget(new ErrorWidget(err_text), TRANSFER_OWNERSHIP);

    return;
  }

  for (int i = 0; i < m_stages->count(); i++) {
    m_stages->filterAt(i)->loadDefaultSettings(page);
  }

  if (!isBatchProcessingInProgress()) {
    if (m_imageFrameLayout->indexOf(m_processingIndicationWidget.get()) != -1) {
      m_processingIndicationWidget->processingRestartedEffect();
    }
    bool current_widget_is_image = (Utils::castOrFindChild<ImageViewBase*>(m_imageFrameLayout->widget(0)) != nullptr);
    setImageWidget(m_processingIndicationWidget.get(), KEEP_OWNERSHIP, nullptr, current_widget_is_image);
    m_stages->filterAt(m_curFilter)->preUpdateUI(this, page);
  }

  assert(m_thumbnailCache);

  m_interactiveQueue->cancelAndClear();
  m_interactiveQueue->addProcessingTask(page, createCompositeTask(page, m_curFilter, /*batch=*/false, m_debug));
  m_workerThreadPool->submitTask(m_interactiveQueue->takeForProcessing());
}  // MainWindow::loadPageInteractive

void MainWindow::updateWindowTitle() {
  QString project_name;
  CommandLine cli = CommandLine::get();

  if (m_projectFile.isEmpty()) {
    project_name = tr("Unnamed");
  } else if (cli.hasWindowTitle()) {
    project_name = cli.getWindowTitle();
  } else {
    project_name = QFileInfo(m_projectFile).completeBaseName();
  }
  const QString version(QString::fromUtf8(VERSION));
  setWindowTitle(tr("%2 - ScanTailor Advanced [%1bit]").arg(sizeof(void*) * 8).arg(project_name));
}

/**
 * \brief Closes the currently project, prompting to save it if necessary.
 *
 * \return true if the project was closed, false if the user cancelled the process.
 */
bool MainWindow::closeProjectInteractive() {
  if (!isProjectLoaded()) {
    return true;
  }

  if (m_projectFile.isEmpty()) {
    switch (promptProjectSave()) {
      case SAVE:
        saveProjectTriggered();
        // fall through
      case DONT_SAVE:
        break;
      case CANCEL:
        return false;
    }
    closeProjectWithoutSaving();

    return true;
  }

  const QFileInfo project_file(m_projectFile);
  const QFileInfo backup_file(project_file.absoluteDir(), QString::fromLatin1("Backup.") + project_file.fileName());
  const QString backup_file_path(backup_file.absoluteFilePath());

  ProjectWriter writer(m_pages, m_selectedPage, m_outFileNameGen);

  if (!writer.write(backup_file_path, m_stages->filters())) {
    // Backup file could not be written???
    QFile::remove(backup_file_path);
    switch (promptProjectSave()) {
      case SAVE:
        saveProjectTriggered();
        // fall through
      case DONT_SAVE:
        break;
      case CANCEL:
        return false;
    }
    closeProjectWithoutSaving();

    return true;
  }

  if (compareFiles(m_projectFile, backup_file_path)) {
    // The project hasn't really changed.
    QFile::remove(backup_file_path);
    closeProjectWithoutSaving();

    return true;
  }

  switch (promptProjectSave()) {
    case SAVE:
      if (!Utils::overwritingRename(backup_file_path, m_projectFile)) {
        QMessageBox::warning(this, tr("Error"), tr("Error saving the project file!"));

        return false;
      }
      // fall through
    case DONT_SAVE:
      QFile::remove(backup_file_path);
      break;
    case CANCEL:
      return false;
  }

  closeProjectWithoutSaving();

  return true;
}  // MainWindow::closeProjectInteractive

void MainWindow::closeProjectWithoutSaving() {
  auto pages = make_intrusive<ProjectPages>();
  switchToNewProject(pages, QString());
}

bool MainWindow::saveProjectWithFeedback(const QString& project_file) {
  ProjectWriter writer(m_pages, m_selectedPage, m_outFileNameGen);

  if (!writer.write(project_file, m_stages->filters())) {
    QMessageBox::warning(this, tr("Error"), tr("Error saving the project file!"));

    return false;
  }

  return true;
}

/**
 * Note: showInsertFileDialog(BEFORE, ImageId()) is legal and means inserting at the end.
 */
void MainWindow::showInsertFileDialog(BeforeOrAfter before_or_after, const ImageId& existing) {
  if (isBatchProcessingInProgress() || !isProjectLoaded()) {
    return;
  }
  // We need to filter out files already in project.
  class ProxyModel : public QSortFilterProxyModel {
   public:
    ProxyModel(const ProjectPages& pages) {
      setDynamicSortFilter(true);

      const PageSequence sequence(pages.toPageSequence(IMAGE_VIEW));
      for (const PageInfo& page : sequence) {
        m_inProjectFiles.push_back(QFileInfo(page.imageId().filePath()));
      }
    }

   protected:
    virtual bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
      const QModelIndex idx(source_parent.child(source_row, 0));
      const QVariant data(idx.data(QFileSystemModel::FilePathRole));
      if (data.isNull()) {
        return true;
      }

      return !m_inProjectFiles.contains(QFileInfo(data.toString()));
    }

    virtual bool lessThan(const QModelIndex& left, const QModelIndex& right) const { return left.row() < right.row(); }

   private:
    QFileInfoList m_inProjectFiles;
  };


  std::unique_ptr<QFileDialog> dialog(
      new QFileDialog(this, tr("Files to insert"), QFileInfo(existing.filePath()).absolutePath()));
  dialog->setFileMode(QFileDialog::ExistingFiles);
  dialog->setProxyModel(new ProxyModel(*m_pages));
  dialog->setNameFilter(tr("Images not in project (%1)").arg("*.png *.tiff *.tif *.jpeg *.jpg"));
  // XXX: Adding individual pages from a multi-page TIFF where
  // some of the pages are already in project is not supported right now.
  if (dialog->exec() != QDialog::Accepted) {
    return;
  }

  QStringList files(dialog->selectedFiles());
  if (files.empty()) {
    return;
  }

  // The order of items returned by QFileDialog is platform-dependent,
  // so we enforce our own ordering.
  std::sort(files.begin(), files.end(), SmartFilenameOrdering());

  // I suspect on some platforms it may be possible to select the same file twice,
  // so to be safe, remove duplicates.
  files.erase(std::unique(files.begin(), files.end()), files.end());


  std::vector<ImageFileInfo> new_files;
  std::vector<QString> loaded_files;
  std::vector<QString> failed_files;  // Those we failed to read metadata from.
  // dialog->selectedFiles() returns file list in reverse order.
  for (int i = files.size() - 1; i >= 0; --i) {
    const QFileInfo file_info(files[i]);
    ImageFileInfo image_file_info(file_info, std::vector<ImageMetadata>());

    const ImageMetadataLoader::Status status = ImageMetadataLoader::load(
        files.at(i), [&](const ImageMetadata& metadata) { image_file_info.imageInfo().push_back(metadata); });

    if (status == ImageMetadataLoader::LOADED) {
      new_files.push_back(image_file_info);
      loaded_files.push_back(file_info.absoluteFilePath());
    } else {
      failed_files.push_back(file_info.absoluteFilePath());
    }
  }

  if (!failed_files.empty()) {
    std::unique_ptr<LoadFilesStatusDialog> err_dialog(new LoadFilesStatusDialog(this));
    err_dialog->setLoadedFiles(loaded_files);
    err_dialog->setFailedFiles(failed_files);
    err_dialog->setOkButtonName(QString(" %1 ").arg(tr("Skip failed files")));
    if ((err_dialog->exec() != QDialog::Accepted) || loaded_files.empty()) {
      return;
    }
  }

  // Check if there is at least one DPI that's not OK.
  if (std::find_if(new_files.begin(), new_files.end(), [&](ImageFileInfo p) -> bool { return !p.isDpiOK(); })
      != new_files.end()) {
    std::unique_ptr<FixDpiDialog> dpi_dialog(new FixDpiDialog(new_files, this));
    dpi_dialog->setWindowModality(Qt::WindowModal);
    if (dpi_dialog->exec() != QDialog::Accepted) {
      return;
    }

    new_files = dpi_dialog->files();
  }

  // Actually insert the new pages.
  for (const ImageFileInfo& file : new_files) {
    int image_num = -1;  // Zero-based image number in a multi-page TIFF.
    for (const ImageMetadata& metadata : file.imageInfo()) {
      ++image_num;

      const int num_sub_pages = ProjectPages::adviseNumberOfLogicalPages(metadata, OrthogonalRotation());
      const ImageInfo image_info(ImageId(file.fileInfo(), image_num), metadata, num_sub_pages, false, false);
      insertImage(image_info, before_or_after, existing);
    }
  }
}  // MainWindow::showInsertFileDialog

void MainWindow::showRemovePagesDialog(const std::set<PageId>& pages) {
  std::unique_ptr<QDialog> dialog(new QDialog(this));
  Ui::RemovePagesDialog ui;
  ui.setupUi(dialog.get());
  ui.icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxQuestion).pixmap(48, 48));

  ui.text->setText(ui.text->text().arg(pages.size()));

  QPushButton* remove_btn = ui.buttonBox->button(QDialogButtonBox::Ok);
  remove_btn->setText(tr("Remove"));

  dialog->setWindowModality(Qt::WindowModal);
  if (dialog->exec() == QDialog::Accepted) {
    removeFromProject(pages);
    eraseOutputFiles(pages);
  }
}

/**
 * Note: insertImage(..., BEFORE, ImageId()) is legal and means inserting at the end.
 */
void MainWindow::insertImage(const ImageInfo& new_image, BeforeOrAfter before_or_after, ImageId existing) {
  std::vector<PageInfo> pages(m_pages->insertImage(new_image, before_or_after, existing, getCurrentView()));

  if (before_or_after == BEFORE) {
    // The second one will be inserted first, then the first
    // one will be inserted BEFORE the second one.
    std::reverse(pages.begin(), pages.end());
  }

  for (const PageInfo& page_info : pages) {
    m_outFileNameGen.disambiguator()->registerFile(page_info.imageId().filePath());
    m_thumbSequence->insert(page_info, before_or_after, existing);
    existing = page_info.imageId();
  }
}

void MainWindow::removeFromProject(const std::set<PageId>& pages) {
  m_interactiveQueue->cancelAndRemove(pages);
  if (m_batchQueue) {
    m_batchQueue->cancelAndRemove(pages);
  }

  m_pages->removePages(pages);


  const PageSequence itemsInOrder = m_thumbSequence->toPageSequence();
  std::set<PageId> new_selection;

  bool select_first_non_deleted = false;
  if (itemsInOrder.numPages() > 0) {
    // if first page was deleted select first not deleted page
    // otherwise select last not deleted page from beginning
    select_first_non_deleted = pages.find(itemsInOrder.pageAt(0).id()) != pages.end();

    PageId last_non_deleted;
    for (const PageInfo& page : itemsInOrder) {
      const PageId& id = page.id();
      const bool was_deleted = (pages.find(id) != pages.end());

      if (!was_deleted) {
        if (select_first_non_deleted) {
          m_thumbSequence->setSelection(id);
          new_selection.insert(id);
          break;
        } else {
          last_non_deleted = id;
        }
      } else if (!select_first_non_deleted && !last_non_deleted.isNull()) {
        m_thumbSequence->setSelection(last_non_deleted);
        new_selection.insert(last_non_deleted);
        break;
      }
    }

    m_thumbSequence->removePages(pages);

    if (new_selection.empty()) {
      // fallback to old behaviour
      if (m_thumbSequence->selectionLeader().isNull()) {
        m_thumbSequence->setSelection(m_thumbSequence->firstPage().id());
      }
    }
  }

  updateMainArea();
}  // MainWindow::removeFromProject

void MainWindow::eraseOutputFiles(const std::set<PageId>& pages) {
  std::vector<PageId::SubPage> erase_variations;
  erase_variations.reserve(3);

  for (const PageId& page_id : pages) {
    erase_variations.clear();
    switch (page_id.subPage()) {
      case PageId::SINGLE_PAGE:
        erase_variations.push_back(PageId::SINGLE_PAGE);
        erase_variations.push_back(PageId::LEFT_PAGE);
        erase_variations.push_back(PageId::RIGHT_PAGE);
        break;
      case PageId::LEFT_PAGE:
        erase_variations.push_back(PageId::SINGLE_PAGE);
        erase_variations.push_back(PageId::LEFT_PAGE);
        break;
      case PageId::RIGHT_PAGE:
        erase_variations.push_back(PageId::SINGLE_PAGE);
        erase_variations.push_back(PageId::RIGHT_PAGE);
        break;
    }

    for (PageId::SubPage subpage : erase_variations) {
      QFile::remove(m_outFileNameGen.filePathFor(PageId(page_id.imageId(), subpage)));
    }
  }
}

BackgroundTaskPtr MainWindow::createCompositeTask(const PageInfo& page,
                                                  const int last_filter_idx,
                                                  const bool batch,
                                                  bool debug) {
  intrusive_ptr<fix_orientation::Task> fix_orientation_task;
  intrusive_ptr<page_split::Task> page_split_task;
  intrusive_ptr<deskew::Task> deskew_task;
  intrusive_ptr<select_content::Task> select_content_task;
  intrusive_ptr<page_layout::Task> page_layout_task;
  intrusive_ptr<output::Task> output_task;

  if (batch) {
    debug = false;
  }

  if (last_filter_idx >= m_stages->outputFilterIdx()) {
    output_task = m_stages->outputFilter()->createTask(page.id(), m_thumbnailCache, m_outFileNameGen, batch, debug);
    debug = false;
  }
  if (last_filter_idx >= m_stages->pageLayoutFilterIdx()) {
    page_layout_task = m_stages->pageLayoutFilter()->createTask(page.id(), output_task, batch, debug);
    debug = false;
  }
  if (last_filter_idx >= m_stages->selectContentFilterIdx()) {
    select_content_task = m_stages->selectContentFilter()->createTask(page.id(), page_layout_task, batch, debug);
    debug = false;
  }
  if (last_filter_idx >= m_stages->deskewFilterIdx()) {
    deskew_task = m_stages->deskewFilter()->createTask(page.id(), select_content_task, batch, debug);
    debug = false;
  }
  if (last_filter_idx >= m_stages->pageSplitFilterIdx()) {
    page_split_task = m_stages->pageSplitFilter()->createTask(page, deskew_task, batch, debug);
    debug = false;
  }
  if (last_filter_idx >= m_stages->fixOrientationFilterIdx()) {
    fix_orientation_task = m_stages->fixOrientationFilter()->createTask(page.id(), page_split_task, batch);
    debug = false;
  }
  assert(fix_orientation_task);

  return make_intrusive<LoadFileTask>(batch ? BackgroundTask::BATCH : BackgroundTask::INTERACTIVE, page,
                                      m_thumbnailCache, m_pages, fix_orientation_task);
}  // MainWindow::createCompositeTask

intrusive_ptr<CompositeCacheDrivenTask> MainWindow::createCompositeCacheDrivenTask(const int last_filter_idx) {
  intrusive_ptr<fix_orientation::CacheDrivenTask> fix_orientation_task;
  intrusive_ptr<page_split::CacheDrivenTask> page_split_task;
  intrusive_ptr<deskew::CacheDrivenTask> deskew_task;
  intrusive_ptr<select_content::CacheDrivenTask> select_content_task;
  intrusive_ptr<page_layout::CacheDrivenTask> page_layout_task;
  intrusive_ptr<output::CacheDrivenTask> output_task;

  if (last_filter_idx >= m_stages->outputFilterIdx()) {
    output_task = m_stages->outputFilter()->createCacheDrivenTask(m_outFileNameGen);
  }
  if (last_filter_idx >= m_stages->pageLayoutFilterIdx()) {
    page_layout_task = m_stages->pageLayoutFilter()->createCacheDrivenTask(output_task);
  }
  if (last_filter_idx >= m_stages->selectContentFilterIdx()) {
    select_content_task = m_stages->selectContentFilter()->createCacheDrivenTask(page_layout_task);
  }
  if (last_filter_idx >= m_stages->deskewFilterIdx()) {
    deskew_task = m_stages->deskewFilter()->createCacheDrivenTask(select_content_task);
  }
  if (last_filter_idx >= m_stages->pageSplitFilterIdx()) {
    page_split_task = m_stages->pageSplitFilter()->createCacheDrivenTask(deskew_task);
  }
  if (last_filter_idx >= m_stages->fixOrientationFilterIdx()) {
    fix_orientation_task = m_stages->fixOrientationFilter()->createCacheDrivenTask(page_split_task);
  }

  assert(fix_orientation_task);

  return fix_orientation_task;
}  // MainWindow::createCompositeCacheDrivenTask

void MainWindow::updateDisambiguationRecords(const PageSequence& pages) {
  for (const PageInfo& page : pages) {
    m_outFileNameGen.disambiguator()->registerFile(page.imageId().filePath());
  }
}

PageSelectionAccessor MainWindow::newPageSelectionAccessor() {
  auto provider = make_intrusive<PageSelectionProviderImpl>(this);

  return PageSelectionAccessor(provider);
}

void MainWindow::changeEvent(QEvent* event) {
  if (event != nullptr) {
    switch (event->type()) {
      case QEvent::LanguageChange:
        retranslateUi(this);
        updateWindowTitle();
        break;
      default:
        QWidget::changeEvent(event);
        break;
    }
  }
}

void MainWindow::setDockWidgetsVisible(bool state) {
  filterDockWidget->setVisible(state);
  thumbnailsDockWidget->setVisible(state);
}

void MainWindow::scaleThumbnails(const QWheelEvent* wheel_event) {
  const QPoint& angle_delta = wheel_event->angleDelta();
  const int wheel_dist = angle_delta.x() + angle_delta.y();

  if (std::abs(wheel_dist) >= 30) {
    const double dx = std::copysign(25.0, wheel_dist);
    const double dy = std::copysign(16.0, wheel_dist);
    const double width = qBound(100.0, m_maxLogicalThumbSize.width() + dx, 1000.0);
    const double height = qBound(64.0, m_maxLogicalThumbSize.height() + dy, 640.0);
    m_maxLogicalThumbSize = QSizeF(width, height);
    if (!m_maxLogicalThumbSizeUpdater.isActive()) {
      m_maxLogicalThumbSizeUpdater.start(350);
    }

    QSettings().setValue("settings/max_logical_thumb_size", m_maxLogicalThumbSize);
  }
}

void MainWindow::updateMaxLogicalThumbSize() {
  m_thumbSequence->setMaxLogicalThumbSize(m_maxLogicalThumbSize);
  updateThumbViewMinWidth();
  resetThumbSequence(currentPageOrderProvider(), ThumbnailSequence::KEEP_SELECTION);
}