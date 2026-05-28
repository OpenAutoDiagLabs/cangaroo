#include "TxGeneratorWindow.h"
#include "ui_TxGeneratorWindow.h"
#include <QTreeWidgetItem>
#include <QTimer>
#include <core/Backend.h>
#include <core/MeasurementNetwork.h>
#include <core/MeasurementSetup.h>
#include <core/MeasurementInterface.h>
#include <driver/CanInterface.h>
#include <driver/CanDriver.h>
#include <sys/time.h>

TxGeneratorWindow::TxGeneratorWindow(QWidget *parent, Backend &backend) :
    ConfigurableWidget(parent),
    ui(new Ui::TxGeneratorWindow),
    _backend(backend)
{
    ui->setupUi(this);

    _sendTimer = new QTimer(this);
    _sendTimer->setInterval(10); // Check every 10ms
    connect(_sendTimer, SIGNAL(timeout()), this, SLOT(onSendTimerTimeout()));
    _sendTimer->start();

    connect(&backend, SIGNAL(onSetupChanged()), this, SLOT(onSetupChanged()));
    connect(&backend, SIGNAL(beginMeasurement()), this, SLOT(refreshInterfaces()));
    connect(&backend, SIGNAL(beginMeasurement()), this, SLOT(updateMeasurementState()));
    connect(&backend, SIGNAL(endMeasurement()), this, SLOT(refreshInterfaces()));
    connect(&backend, SIGNAL(endMeasurement()), this, SLOT(updateMeasurementState()));

    connect(ui->btnBulkRun, SIGNAL(clicked()), this, SLOT(on_btnBulkRun_clicked()));
    connect(ui->btnBulkStop, SIGNAL(clicked()), this, SLOT(on_btnBulkStop_clicked()));
    
    // Initial styling
    ui->btnBulkRun->setStyleSheet("QPushButton { font-weight: bold; } QPushButton:checked { background-color: #28a745; color: white; border: 1px solid #218838; }");
    ui->btnBulkStop->setStyleSheet("QPushButton { font-weight: bold; } QPushButton:checked { background-color: #dc3545; color: white; border: 1px solid #c82333; }");
    
    connect(ui->treeActive, SIGNAL(itemChanged(QTreeWidgetItem*,int)), this, SLOT(on_treeActive_itemChanged(QTreeWidgetItem*,int)));
    connect(ui->treeAvailable, SIGNAL(itemSelectionChanged()), this, SLOT(on_treeAvailable_itemSelectionChanged()));

    _bitMatrixWidget = new BitMatrixWidget(this);
    // REMOVED redundant addWidget to verticalLayoutTabLayout
    
    // Configure Scroll Area
    ui->scrollAreaLayout->setWidgetResizable(false);
    ui->scrollAreaLayout->setWidget(_bitMatrixWidget);
    
    // Initialize Layout View controls
    ui->sliderLayoutZoom->setRange(30, 120);
    ui->sliderLayoutZoom->setValue(50);
    _bitMatrixWidget->setCellSize(50);
    _bitMatrixWidget->setFixedSize(_bitMatrixWidget->sizeHint());

    ui->lineManualId->setInputMask("");
    ui->lineManualId->setValidator(new QRegularExpressionValidator(QRegularExpression("^[0-9A-Fa-f]{0,8}$"), this));
    connect(ui->lineManualId, &QLineEdit::textChanged, this, [this](const QString &text){
        if (text != text.toUpper()) {
            int cursorPos = ui->lineManualId->cursorPosition();
            ui->lineManualId->setText(text.toUpper());
            ui->lineManualId->setCursorPosition(cursorPos);
        }
    });

    ui->treeActive->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->treeAvailable->setSelectionMode(QAbstractItemView::ExtendedSelection);

    // Add Random Payload button programmatically
    _btnRandomPayload = new QPushButton(tr("🎲 Randomize Payload"), this);
    _btnRandomPayload->setToolTip(tr("Randomize data bytes for selected messages"));
    _btnRandomPayload->setStyleSheet("QPushButton { font-weight: bold; background: #6f42c1; color: white; border-radius: 4px; padding: 4px 8px; } QPushButton:hover { background: #5a32a3; }");
    ui->horizontalLayoutActiveControls->insertWidget(3, _btnRandomPayload); // Insert next to Run/Stop
    connect(_btnRandomPayload, &QPushButton::released, this, &TxGeneratorWindow::onRandomPayloadReleased);

    srand(time(NULL));

    refreshInterfaces();
    updateMeasurementState();
    populateDbcMessages();
    updateActiveList();
    isLoading = false;
}

TxGeneratorWindow::~TxGeneratorWindow()
{
    delete ui;
}

bool TxGeneratorWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    if (!ConfigurableWidget::saveXML(backend, xml, root)) { return false; }
    root.setAttribute("type", "TxGeneratorWindow");
    return true;
}

bool TxGeneratorWindow::loadXML(Backend &backend, QDomElement &el)
{
    if (!ConfigurableWidget::loadXML(backend, el)) { return false; }
    return true;
}

void TxGeneratorWindow::refreshInterfaces()
{
    ui->comboBoxInterface->blockSignals(true);
    ui->comboBoxInterface->clear();
    
    MeasurementSetup &setup = _backend.getSetup();
    foreach (MeasurementNetwork *network, setup.getNetworks()) {
        foreach (MeasurementInterface *mi, network->interfaces()) {
            CanInterfaceId ifid = mi->canInterface();
            CanInterface *intf = _backend.getInterfaceById(ifid);
            if (intf) {
                QString name = network->name() + ": " + intf->getName();
                ui->comboBoxInterface->addItem(name, QVariant(ifid));
            }
        }
    }
    if (ui->comboBoxInterface->count() > 0 && ui->comboBoxInterface->currentIndex() == -1) {
        ui->comboBoxInterface->setCurrentIndex(0);
    }
    ui->comboBoxInterface->blockSignals(false);
    populateDbcMessages();
}

void TxGeneratorWindow::populateDbcMessages()
{
    ui->treeAvailable->clear();
    
    CanInterfaceId currentId = (CanInterfaceId)ui->comboBoxInterface->currentData().toUInt();
    MeasurementSetup &setup = _backend.getSetup();

    foreach (MeasurementNetwork *network, setup.getNetworks()) {
        // Only show DBCs associated with the current interface if possible, 
        // but currently networks map to interfaces. 
        // Let's find if this network is using our interface.
        bool interfaceMatches = false;
        foreach (MeasurementInterface *mi, network->interfaces()) {
            if (mi->canInterface() == currentId) {
                interfaceMatches = true;
                break;
            }
        }

        if (interfaceMatches) {
            foreach (pCanDb db, network->_canDbs) {
                if (db) {
                    CanDbMessageList msgs = db->getMessageList();
                    for (auto it = msgs.begin(); it != msgs.end(); ++it) {
                        CanDbMessage *dbMsg = *it;
                        if (dbMsg) {
                            QTreeWidgetItem *item = new QTreeWidgetItem(ui->treeAvailable);
                            item->setText(0, "0x" + QString("%1").arg(dbMsg->getRaw_id(), 3, 16, QChar('0')).toUpper());
                            item->setText(1, dbMsg->getName());
                            item->setData(0, Qt::UserRole, QVariant::fromValue((void*)dbMsg));
                        }
                    }
                }
            }
        }
    }
}

void TxGeneratorWindow::on_lineEditSearchAvailable_textChanged(const QString &text)
{
    for (int i = 0; i < ui->treeAvailable->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = ui->treeAvailable->topLevelItem(i);
        bool match = item->text(0).contains(text, Qt::CaseInsensitive) || item->text(1).contains(text, Qt::CaseInsensitive);
        item->setHidden(!match);
    }
}

void TxGeneratorWindow::on_treeAvailable_itemSelectionChanged()
{
    QTreeWidgetItem *item = ui->treeAvailable->currentItem();
    if (item && _bitMatrixWidget) {
        CanDbMessage *dbMsg = (CanDbMessage*)item->data(0, Qt::UserRole).value<void*>();
        _bitMatrixWidget->setMessage(dbMsg);
    } else if (_bitMatrixWidget) {
        _bitMatrixWidget->setMessage(nullptr);
    }
}

void TxGeneratorWindow::on_sliderLayoutZoom_valueChanged(int value)
{
    if (_bitMatrixWidget) {
        _bitMatrixWidget->setCellSize(value);
        _bitMatrixWidget->setFixedSize(_bitMatrixWidget->sizeHint());
    }
}

void TxGeneratorWindow::on_cbLayoutCompact_toggled(bool checked)
{
    if (_bitMatrixWidget) {
        _bitMatrixWidget->setCompactMode(checked);
        _bitMatrixWidget->setFixedSize(_bitMatrixWidget->sizeHint());
    }
}

void TxGeneratorWindow::on_btnAddToList_released()
{
    QList<QTreeWidgetItem*> selected = ui->treeAvailable->selectedItems();
    if (selected.isEmpty()) {
        QTreeWidgetItem *current = ui->treeAvailable->currentItem();
        if (current) selected.append(current);
    }

    if (selected.isEmpty()) return;

    foreach (QTreeWidgetItem *item, selected) {
        CanDbMessage *dbMsg = (CanDbMessage*)item->data(0, Qt::UserRole).value<void*>();
        if (!dbMsg) continue;

        CyclicMessage cm;
        cm.msg = CanMessage(); // Ensure fresh instance
        cm.msg.setId(dbMsg->getRaw_id());
        cm.msg.setLength(dbMsg->getDlc());
        cm.msg.setExtended(dbMsg->getRaw_id() > 0x7FF);
        cm.name = dbMsg->getName();
        cm.interval = 100;
        cm.enabled = false;
        cm.lastSent = 0;
        cm.interfaceId = (CanInterfaceId)ui->comboBoxInterface->currentData().toUInt();
        cm.dbMsg = dbMsg;

        _cyclicMessages.append(cm);
    }
    updateActiveList();
    ui->treeActive->scrollToBottom();
}

void TxGeneratorWindow::on_btnAddManual_released()
{
    bool ok;
    uint32_t id = ui->lineManualId->text().toUInt(&ok, 16);
    if (!ok) return;

    CyclicMessage cm;
    cm.msg = CanMessage(); // Ensure fresh instance
    cm.msg.setId(id);
    cm.msg.setLength(ui->spinManualDlc->value());
    cm.msg.setExtended(id > 0x7FF || ui->lineManualId->text().length() > 3);
    cm.msg.setFD(ui->spinManualDlc->value() > 8);
    cm.name = "Manual";
    cm.interval = 100;
    cm.enabled = false;
    cm.lastSent = 0;
    cm.interfaceId = (CanInterfaceId)ui->comboBoxInterface->currentData().toUInt();
    cm.dbMsg = nullptr;

    _cyclicMessages.append(cm);
    updateActiveList();
    ui->treeActive->scrollToBottom();
}

void TxGeneratorWindow::on_btnRemove_released()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) return;

    // To avoid index shifting issues, we collect rows and remove from highest to lowest
    QList<int> rows;
    foreach (QTreeWidgetItem *item, selected) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0) rows.append(row);
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());

    foreach (int row, rows) {
        if (row >= 0 && row < _cyclicMessages.size()) {
            _cyclicMessages.removeAt(row);
        }
    }
    updateActiveList();
}

void TxGeneratorWindow::on_btnSendOnce_released()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) {
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            selected.append(ui->treeActive->currentItem());
        }
    }

    foreach (QTreeWidgetItem *item, selected) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0 && row < _cyclicMessages.size()) {
            CyclicMessage &cm = _cyclicMessages[row];
            CanInterface *intf = _backend.getInterfaceById(cm.interfaceId);
            if (intf && intf->isOpen()) {
                cm.msg.setInterfaceId(cm.interfaceId);
                intf->sendMessage(cm.msg);
                if (ui->cbShowInTrace->isChecked()) {
                    CanMessage loopback = cm.msg;
                    loopback.setRX(false);
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    loopback.setTimestamp(tv);
                    emit loopbackFrame(loopback);
                }
            } else {
                QString errorMsg = QString("TxGeneratorWindow: Interface %1 is not open.").arg(intf ? intf->getName() : QString::number(cm.interfaceId));
                log_error(errorMsg);
            }
        }
    }
}

void TxGeneratorWindow::on_btnBulkRun_clicked()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) return;

    foreach (QTreeWidgetItem *item, selected) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0 && row < _cyclicMessages.size()) {
            _cyclicMessages[row].enabled = true;
            updateRowUI(row);
        }
    }
    
    ui->btnBulkRun->setChecked(true);
    ui->btnBulkStop->setChecked(false);
}

void TxGeneratorWindow::on_btnBulkStop_clicked()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) {
        // Fallback to active index if nothing selected
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            _cyclicMessages[row].enabled = false;
            updateRowUI(row);
        }
    } else {
        foreach (QTreeWidgetItem *item, selected) {
            int row = ui->treeActive->indexOfTopLevelItem(item);
            if (row >= 0 && row < _cyclicMessages.size()) {
                _cyclicMessages[row].enabled = false;
                updateRowUI(row);
            }
        }
    }
    
    ui->btnBulkRun->setChecked(false);
    ui->btnBulkStop->setChecked(true);
}

void TxGeneratorWindow::on_spinInterval_valueChanged(int i)
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) {
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            _cyclicMessages[row].interval = i;
            updateRowUI(row);
        }
    } else {
        foreach (QTreeWidgetItem *item, selected) {
            int row = ui->treeActive->indexOfTopLevelItem(item);
            if (row >= 0 && row < _cyclicMessages.size()) {
                _cyclicMessages[row].interval = i;
                updateRowUI(row);
            }
        }
    }
}

void TxGeneratorWindow::on_comboBoxInterface_currentIndexChanged(int index)
{
    (void)index;
    populateDbcMessages();
    emit interfaceChanged((CanInterfaceId)ui->comboBoxInterface->currentData().toUInt());
}

void TxGeneratorWindow::on_treeAvailable_itemDoubleClicked(QTreeWidgetItem *item, int column)
{
    (void)column;
    on_btnAddToList_released();
}

void TxGeneratorWindow::on_treeActive_itemSelectionChanged()
{
    isLoading = true;
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (!selected.isEmpty()) {
        int row = ui->treeActive->indexOfTopLevelItem(selected.first());
        if (row >= 0 && row < _cyclicMessages.size()) {
            const CyclicMessage &cm = _cyclicMessages[row];
            
            ui->btnBulkRun->blockSignals(true);
            ui->btnBulkStop->blockSignals(true);
            ui->btnBulkRun->setChecked(cm.enabled);
            ui->btnBulkStop->setChecked(!cm.enabled);
            ui->btnBulkRun->blockSignals(false);
            ui->btnBulkStop->blockSignals(false);
            
            ui->spinInterval->blockSignals(true);
            ui->spinInterval->setValue(cm.interval);
            ui->spinInterval->blockSignals(false);
            
            emit messageSelected(cm.msg, cm.name, cm.interfaceId, cm.dbMsg);
        }
    } else {
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            const CyclicMessage &cm = _cyclicMessages[row];
            ui->btnBulkRun->blockSignals(true);
            ui->btnBulkStop->blockSignals(true);
            ui->btnBulkRun->setChecked(cm.enabled);
            ui->btnBulkStop->setChecked(!cm.enabled);
            ui->btnBulkRun->blockSignals(false);
            ui->btnBulkStop->blockSignals(false);
            
            ui->spinInterval->blockSignals(true);
            ui->spinInterval->setValue(cm.interval);
            ui->spinInterval->blockSignals(false);
            
            emit messageSelected(cm.msg, cm.name, cm.interfaceId, cm.dbMsg);
        }
    }
    isLoading = false;
}

void TxGeneratorWindow::on_treeActive_itemChanged(QTreeWidgetItem *item, int column)
{
    int row = ui->treeActive->indexOfTopLevelItem(item);
    if (row >= 0 && row < _cyclicMessages.size()) {
        if (column == 5) {
            bool ok;
            int interval = item->text(5).toInt(&ok);
            if (ok && interval > 0 && _cyclicMessages[row].interval != interval) {
                _cyclicMessages[row].interval = interval;
                
                // If this item is part of a selection, apply to all selected items
                if (item->isSelected()) {
                    foreach (QTreeWidgetItem *selItem, ui->treeActive->selectedItems()) {
                        if (selItem == item) continue;
                        int selRow = ui->treeActive->indexOfTopLevelItem(selItem);
                        if (selRow >= 0 && selRow < _cyclicMessages.size()) {
                            _cyclicMessages[selRow].interval = interval;
                            updateRowUI(selRow);
                        }
                    }
                }
            }
        }
    }
}

void TxGeneratorWindow::on_btnSelectAll_released()
{
    ui->treeActive->selectAll();
}

void TxGeneratorWindow::on_btnClearAll_released()
{
    ui->treeActive->clearSelection();
}

void TxGeneratorWindow::onStatusButtonClicked()
{
    QPushButton *btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    // Determine the row of the button
    QPoint pos = btn->parentWidget()->mapTo(ui->treeActive->viewport(), btn->pos());
    QTreeWidgetItem *item = ui->treeActive->itemAt(pos);
    if (!item) return;

    int row = ui->treeActive->indexOfTopLevelItem(item);
    if (row >= 0 && row < _cyclicMessages.size()) {
        bool targetState = !_cyclicMessages[row].enabled;
        
        // If this item is part of a selection, apply to all selected items
        if (item->isSelected()) {
            foreach (QTreeWidgetItem *selItem, ui->treeActive->selectedItems()) {
                int selRow = ui->treeActive->indexOfTopLevelItem(selItem);
                if (selRow >= 0 && selRow < _cyclicMessages.size()) {
                    _cyclicMessages[selRow].enabled = targetState;
                    updateRowUI(selRow);
                }
            }
        } else {
            _cyclicMessages[row].enabled = targetState;
            updateRowUI(row);
        }
    }
}

void TxGeneratorWindow::onSendTimerTimeout()
{
    if (!_backend.isMeasurementRunning()) {
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now_ms = (uint64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);

    for (int i = 0; i < _cyclicMessages.size(); ++i) {
        CyclicMessage &cm = _cyclicMessages[i];
        if (cm.enabled && (now_ms - cm.lastSent >= (uint64_t)cm.interval)) {
            CanInterface *intf = _backend.getInterfaceById(cm.interfaceId);
            if (intf && intf->isOpen()) {
                cm.msg.setInterfaceId(cm.interfaceId);
                intf->sendMessage(cm.msg);
                if (ui->cbShowInTrace->isChecked()) {
                    CanMessage loopback = cm.msg;
                    loopback.setRX(false);
                    struct timeval tv_loop;
                    gettimeofday(&tv_loop, NULL);
                    loopback.setTimestamp(tv_loop);
                    emit loopbackFrame(loopback);
                }
                cm.lastSent = now_ms;
            } else {
                QString errorMsg = QString("TxGeneratorWindow: Cyclic - Interface %1 is not open.").arg(intf ? intf->getName() : QString::number(cm.interfaceId));
                if (!_backend.isMeasurementRunning()) {
                    errorMsg += " Did you start the measurement?";
                }
                log_error(errorMsg);
            }
        }
    }
}

void TxGeneratorWindow::onSetupChanged()
{
    refreshInterfaces();
}

void TxGeneratorWindow::updateMeasurementState()
{
    bool running = _backend.isMeasurementRunning();
    ui->btnSendOnce->setEnabled(running);
    ui->groupBoxActive->setEnabled(running);
    if (!running) {
        stopAll();
    }
}

void TxGeneratorWindow::updateActiveList()
{
    // Save selection
    QList<int> selectedRows;
    foreach (QTreeWidgetItem *item, ui->treeActive->selectedItems()) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0) selectedRows.append(row);
    }
    int currentRow = ui->treeActive->currentIndex().row();

    ui->treeActive->blockSignals(true);
    ui->treeActive->clear();
    for (int i = 0; i < _cyclicMessages.size(); ++i) {
        const CyclicMessage &cm = _cyclicMessages[i];
        QTreeWidgetItem *item = new QTreeWidgetItem(ui->treeActive);
        
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable); // Explicitly remove checkbox
        
        // Create Status Button
        QPushButton *btnStatus = new QPushButton(cm.enabled ? "⏹" : "▶");
        btnStatus->setToolTip(cm.enabled ? "Stop" : "Start");
        btnStatus->setFixedWidth(40);
        if (cm.enabled) {
            btnStatus->setStyleSheet("QPushButton { color: #dc3545; font-weight: bold; background: transparent; border: 1px solid #dc3545; border-radius: 3px; } QPushButton:hover { background: #dc3545; color: white; }");
        } else {
            btnStatus->setStyleSheet("QPushButton { color: #28a745; font-weight: bold; background: transparent; border: 1px solid #28a745; border-radius: 3px; } QPushButton:hover { background: #28a745; color: white; }");
        }
        
        connect(btnStatus, &QPushButton::clicked, this, &TxGeneratorWindow::onStatusButtonClicked);
        ui->treeActive->setItemWidget(item, 0, btnStatus);
        
        item->setText(1, "0x" + QString("%1").arg(cm.msg.getId(), 3, 16, QChar('0')).toUpper());
        item->setText(2, cm.name);
        CanInterface *intf = _backend.getInterfaceById(cm.interfaceId);
        item->setText(3, intf ? intf->getName() : "Unknown");
        item->setText(4, QString::number(cm.msg.getLength()));
        item->setText(5, QString::number(cm.interval));

        if (selectedRows.contains(i)) {
            item->setSelected(true);
        }
        if (i == currentRow) {
            ui->treeActive->setCurrentItem(item);
        }
    }
    ui->treeActive->blockSignals(false);
}

void TxGeneratorWindow::updateRowUI(int row)
{
    if (row < 0 || row >= _cyclicMessages.size()) return;
    QTreeWidgetItem *item = ui->treeActive->topLevelItem(row);
    if (!item) return;

    const CyclicMessage &cm = _cyclicMessages[row];
    
    ui->treeActive->blockSignals(true);
    
    // Update button in column 0
    QPushButton *btnStatus = qobject_cast<QPushButton*>(ui->treeActive->itemWidget(item, 0));
    if (btnStatus) {
        btnStatus->setText(cm.enabled ? "⏹" : "▶");
        btnStatus->setToolTip(cm.enabled ? "Stop" : "Start");
        if (cm.enabled) {
            btnStatus->setStyleSheet("QPushButton { color: #dc3545; font-weight: bold; background: transparent; border: 1px solid #dc3545; border-radius: 3px; } QPushButton:hover { background: #dc3545; color: white; }");
        } else {
            btnStatus->setStyleSheet("QPushButton { color: #28a745; font-weight: bold; background: transparent; border: 1px solid #28a745; border-radius: 3px; } QPushButton:hover { background: #28a745; color: white; }");
        }
    }
    
    item->setText(1, "0x" + QString("%1").arg(cm.msg.getId(), 3, 16, QChar('0')).toUpper());
    item->setText(2, cm.name);
    CanInterface *intf = _backend.getInterfaceById(cm.interfaceId);
    item->setText(3, intf ? intf->getName() : "Unknown");
    item->setText(4, QString::number(cm.msg.getLength()));
    item->setText(5, QString::number(cm.interval));

    ui->treeActive->blockSignals(false);
}


void TxGeneratorWindow::updateMessage(const CanMessage &msg)
{
    if (isLoading) return;

    int row = ui->treeActive->currentIndex().row();
    if (row >= 0 && row < _cyclicMessages.size()) {
        _cyclicMessages[row].msg = msg;
        // Also update the tree item text if ID changed
        QTreeWidgetItem *item = ui->treeActive->topLevelItem(row);
        if (item) {
            item->setText(1, "0x" + QString("%1").arg(msg.getId(), 3, 16, QChar('0')).toUpper());
            item->setText(4, QString::number(msg.getLength()));
        }
    }
}

void TxGeneratorWindow::stopAll()
{
    for (int i = 0; i < _cyclicMessages.size(); ++i) {
        _cyclicMessages[i].enabled = false;
        updateRowUI(i);
    }
    ui->btnBulkRun->setChecked(false);
    ui->btnBulkStop->setChecked(false);
}

QSize TxGeneratorWindow::sizeHint() const
{
    return QSize(1200, 600);
}

void TxGeneratorWindow::onRandomPayloadReleased()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) {
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            selected.append(ui->treeActive->currentItem());
        }
    }

    foreach (QTreeWidgetItem *item, selected) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0 && row < _cyclicMessages.size()) {
            CyclicMessage &cm = _cyclicMessages[row];
            for (int i = 0; i < cm.msg.getLength(); ++i) {
                cm.msg.setDataAt(i, (uint8_t)(rand() % 256));
            }
            updateRowUI(row);
            
            // If this is the currently focused message in the bit matrix, update it
            if (item == ui->treeActive->currentItem()) {
                emit messageSelected(cm.msg, cm.name, cm.interfaceId, cm.dbMsg);
            }
        }
    }
}
