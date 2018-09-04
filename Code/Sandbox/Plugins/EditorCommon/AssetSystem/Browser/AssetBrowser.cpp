// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.
#include "StdAfx.h"
#include "AssetBrowser.h"
#include "AssetDropHandler.h"
#include "AssetReverseDependenciesDialog.h"

#include "AssetSystem/Asset.h"
#include "AssetSystem/AssetManager.h"
#include "AssetSystem/EditableAsset.h"
#include "AssetSystem/AssetImporter.h"
#include "AssetSystem/AssetResourceSelector.h"

#include "AssetModel.h"
#include "NewAssetModel.h"
#include "AssetFoldersModel.h"
#include "AssetFolderFilterModel.h"
#include "AssetFoldersView.h"
#include "AssetTooltip.h"
#include "AssetThumbnailsLoader.h"
#include "AssetThumbnailsGenerator.h"
#include "LineEditDelegate.h"

#include "Menu/MenuWidgetBuilders.h"
#include "DragDrop.h"
#include "ThreadingUtils.h"
#include "FilePathUtil.h"
#include "QtUtil.h"
#include "QtViewPane.h"
#include "QAdvancedTreeView.h"
#include "QAdvancedItemDelegate.h"
#include "QThumbnailView.h"
#include "QFilteringPanel.h"
#include "QControls.h"
#include "ProxyModels/AttributeFilterProxyModel.h"
#include "FileDialogs/SystemFileDialog.h"
#include "Controls/BreadcrumbsBar.h"
#include "Notifications/NotificationCenter.h"

#include <QDirIterator>
#include <QDragEnterEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QSplitter>
#include <QGridLayout>
#include <QListView>
#include <QButtonGroup>

REGISTER_VIEWPANE_FACTORY(CAssetBrowser, "Asset Browser", "Tools", false);

CCrySignal<void(CAbstractMenu&, const std::shared_ptr<IUIContext>&)> CAssetBrowser::s_signalMenuCreated;
CCrySignal<void(CAbstractMenu&, const std::vector<CAsset*>&, const std::vector<string>& folders, const std::shared_ptr<IUIContext>&)> CAssetBrowser::s_signalContextMenuRequested;

namespace Private_AssetBrowser
{

//! returns EAssetModelRowType
static int GetType(const QModelIndex& index)
{
	return (EAssetModelRowType)index.data((int)CAssetModel::Roles::TypeCheckRole).toUInt();
}

static bool IsAsset(const QModelIndex& index)
{
	bool ok = false;
	return index.data((int)CAssetModel::Roles::TypeCheckRole).toUInt(&ok) == eAssetModelRow_Asset && ok;
}

static bool IsFolder(const QModelIndex& index)
{
	bool ok = false;
	return index.data((int)CAssetModel::Roles::TypeCheckRole).toUInt(&ok) == eAssetModelRow_Folder && ok;
}

static CAsset* ToAsset(const QModelIndex& index)
{
	return reinterpret_cast<CAsset*>(index.data((int)CAssetModel::Roles::InternalPointerRole).value<intptr_t>());
}

static QString ToFolderPath(const QModelIndex& index)
{
	return index.data((int)CAssetFoldersModel::Roles::FolderPathRole).toString();
}

static bool UserConfirmsRenaming(CAsset& asset, QWidget* pParent)
{
	const CAssetManager* const pAssetManager = CAssetManager::GetInstance();
	const QString question = QObject::tr("There is a possibility that %1 has undetected dependencies which can be violated after the operation.\n"
	                                     "\n"
	                                     "Do you really want to rename the asset?").arg(QtUtil::ToQString(asset.GetName()));
	const QString title = QObject::tr("Rename Asset");

	if (pAssetManager->HasAnyReverseDependencies({ &asset }))
	{
		CAssetReverseDependenciesDialog dialog({ &asset },
		                                       QObject::tr("Asset to be renamed"),
		                                       QObject::tr("Dependent Assets"),
		                                       QObject::tr("The following assets probably will not behave correctly after performing the operation."),
		                                       question, pParent);
		dialog.setWindowTitle(title);

		if (!dialog.Execute())
		{
			return false;
		}
	}
	else if (CQuestionDialog::SQuestion(title, question) != QDialogButtonBox::Yes)
	{
		return false;
	}

	return true;
}

// A private helper class that allows to drop items into the root folder of details and thumbnails views.
// This is a workaround for CAssetFolderFilterModel that does not support hierarchy so the views can not use QAbstractItemView::SetRootIndex()
template <typename TView> 
class CDraggingIntoRootOf : public TView
{
public:
	template<typename... Arg> 
	CDraggingIntoRootOf(Arg&&... arg) 
		: TView(std::forward<Arg>(arg)...)
	{
	}

	// The root folder is the parent folder to the view's top level items.
	// folder.IsEmpty is a valid string and stands for the assets root folder, while folder.isNull means the root folder is not assigned.
	void SetRootFolder(const QString& folder)
	{
		m_root = folder;
	}

	template<typename TEvent>
	bool Processed(const TEvent* pEvent)
	{
		return pEvent->isAccepted() || m_root.isNull() || indexAt(pEvent->pos()).isValid();
	}

	template<typename TEvent>
	void dragEnterMoveRoot(TEvent* pEvent) const
	{
		const CAssetFoldersModel* const pModel = CAssetFoldersModel::GetInstance();
		const QModelIndex root = pModel->FindIndexForFolder(m_root);
		if (pModel->canDropMimeData(pEvent->mimeData(), pEvent->dropAction(), root.row(), root.column(), root.parent()))
		{
			pEvent->accept();
		}
	}

	virtual void dragEnterEvent(QDragEnterEvent* pEvent) override
	{
		CDragDropData::ShowDragText(qApp->widgetAt(QCursor::pos()), tr("Invalid operation"));

		TView::dragEnterEvent(pEvent);

		if (!Processed(pEvent))
		{
			dragEnterMoveRoot(pEvent);
		}
	}

	virtual void dragMoveEvent(QDragMoveEvent* pEvent) override
	{
		TView::dragMoveEvent(pEvent);

		if (!Processed(pEvent))
		{
			dragEnterMoveRoot(pEvent);
		}
	}


	// For the QListView (Thumbnail view) we want to use QListView::Movement::Static but this will disable drag&drop.
	// Calling here the QAbstractItemView implementation directly, we disable the items movement for the QListView.
	template<typename T = TView, typename std::enable_if<std::is_same<T, QListView>::value, int>::type = 0>
	void BaseDropEvent(QDropEvent* pEvent)
	{
		QAbstractItemView::dropEvent(pEvent);
	}

	template<typename T = TView, typename std::enable_if<!std::is_same<T, QListView>::value, int>::type = 0>
	void BaseDropEvent(QDropEvent* pEvent)
	{
		T::dropEvent(pEvent);
	}


	virtual void dropEvent(QDropEvent* pEvent) override
	{
		BaseDropEvent(pEvent);

		if (!Processed(pEvent))
		{
			CAssetFoldersModel* const pModel = CAssetFoldersModel::GetInstance();
			const QModelIndex root = pModel->FindIndexForFolder(m_root);
			if (pModel->dropMimeData(pEvent->mimeData(), pEvent->dropAction(), root.row(), root.column(), root.parent()))
			{
				pEvent->accept();
			}
		}
	}
private:
	QString m_root;
};

class QAssetDetailsView : public CDraggingIntoRootOf<QAdvancedTreeView>
{
public:
	QAssetDetailsView(QWidget* parent = nullptr)
		: CDraggingIntoRootOf(QAdvancedTreeView::UseItemModelAttribute, parent)
	{
	}
protected:
	virtual bool edit(const QModelIndex& index, EditTrigger trigger, QEvent* pEvent) override
	{
		if ((editTriggers() & trigger) && index.isValid() && IsAsset(index))
		{
			CAsset* pAsset = ToAsset(index);
			if (pAsset && !UserConfirmsRenaming(*pAsset, this))
			{
				if (pEvent)
				{
					pEvent->accept();
				}
				// If return false, Qt can ignore() and propagate the event - it is not what we want.
				return true;
			}
		}
		return QAdvancedTreeView::edit(index, trigger, pEvent);
	}
};

class CThumbnailsInternalView : public CDraggingIntoRootOf<QListView>
{
public:
	CThumbnailsInternalView(QWidget* pParent = nullptr)
		: CDraggingIntoRootOf(pParent)
	{
	}

protected:
	virtual void startDrag(Qt::DropActions supportedActions) override
	{
		if (model() && selectionModel())
		{
			QMimeData* const pMimeData = model()->mimeData(selectionModel()->selectedIndexes());
			CDragDropData::StartDrag(this, supportedActions, pMimeData);
		}
	}

	virtual bool edit(const QModelIndex& index, EditTrigger trigger, QEvent* pEvent) override
	{
		if ((editTriggers() & trigger) && index.isValid() && IsAsset(index))
		{
			CAsset* pAsset = ToAsset(index);
			if (pAsset && !UserConfirmsRenaming(*pAsset, this))
			{
				if (pEvent)
				{
					pEvent->accept();
				}
				return true;
			}
		}
		return QListView::edit(index, trigger, pEvent);
	}

	virtual void scrollContentsBy(int dx, int dy) override
	{
		QListView::scrollContentsBy(dx, dy);
		TouchVisibleAssets();
	}

private:
	void TouchVisibleAssetsBatched(int firstBatchRow)
	{
		if (!model() || !model()->rowCount())
		{
			return;
		}

		const int batchSize = 1 << 3;
		const int lastRow = model()->rowCount() - 1;
		const int lastBatchRow = std::min(lastRow, firstBatchRow + batchSize);
		for (int i = lastBatchRow; i >= firstBatchRow; --i)
		{
			QModelIndex index = model()->index(i, (int)eAssetColumns_Thumbnail);
			if (index.isValid())
			{
				CAsset* const pAsset = (CAsset*)(index.data((int)CAssetModel::Roles::InternalPointerRole).value<intptr_t>());
				if (pAsset && pAsset->GetType()->HasThumbnail())
				{
					const QRect r = visualRect(index);
					if (r.y() > 0 && r.y() < rect().height() && r.size().width() * r.size().height() > 1)
					{
						CAssetThumbnailsLoader::GetInstance().TouchAsset(pAsset);
					}
				}
			}
		}

		if (lastBatchRow < lastRow)
		{
			QPointer<CThumbnailsInternalView> pView(this);
			QTimer::singleShot(0, [pView, lastBatchRow]()
				{
					if (!pView)
					{
					  return;
					}
					pView->TouchVisibleAssetsBatched(lastBatchRow + 1);
				});
		}
	}

	void TouchVisibleAssets()
	{
		TouchVisibleAssetsBatched(0);
	}
};

class CDependenciesOperatorBase : public Attributes::IAttributeFilterOperator
{
	class ResourceSelectionCallback : public IResourceSelectionCallback
	{
	public:
		ResourceSelectionCallback(CAttributeFilter* pFilter, QLineEdit* pLineEdit)
			: m_pFilter(pFilter)
			, m_pLineEdit(pLineEdit)
		{
		}
		virtual void SetValue(const char* szNewValue) override
		{
			const QString path(QtUtil::ToQString(szNewValue));
			m_pLineEdit->setText(path);
			m_pFilter->SetFilterValue(path);
		}
	private:
		CAttributeFilter* const m_pFilter;
		QLineEdit* const        m_pLineEdit;
	};

public:
	virtual QWidget* CreateEditWidget(std::shared_ptr<CAttributeFilter> filter) override
	{
		auto widget = new QWidget();

		QLineEdit* const pLineEdit = new QLineEdit();
		auto currentValue = filter->GetFilterValue();

		if (currentValue.type() == QVariant::String)
		{
			pLineEdit->setText(currentValue.toString());
		}

		QWidget::connect(pLineEdit, &QLineEdit::editingFinished, [pLineEdit, filter]()
			{
				filter->SetFilterValue(pLineEdit->text());
			});

		QToolButton* pButton = new QToolButton();
		pButton->setToolTip(QObject::tr("Open"));
		pButton->setIcon(CryIcon("icons:General/Folder.ico"));
		QWidget::connect(pButton, &QToolButton::clicked, [pLineEdit, filter]()
			{
				ResourceSelectionCallback callback(filter.get(), pLineEdit);
				SResourceSelectorContext context;
				context.callback = &callback;

				const string value(QtUtil::ToString(pLineEdit->text()));
				dll_string filename = SStaticAssetSelectorEntry::SelectFromAsset(context, {}, value.c_str());
				if (!filename.empty() && filename.c_str() != value)
				{
				  callback.SetValue(filename.c_str());
				}
				else // restore the previous value
				{
				  callback.SetValue(value.c_str());
				}
			});

		QHBoxLayout* pLayout = new QHBoxLayout();
		pLayout->setMargin(0);
		pLayout->addWidget(pLineEdit);
		pLayout->addWidget(pButton);
		widget->setLayout(pLayout);
		widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

		return widget;
	}

	virtual void UpdateWidget(QWidget* widget, const QVariant& value) override
	{
		QLineEdit* const pLineEdit = static_cast<QLineEdit*>(widget->layout()->itemAt(0)->widget());
		if (pLineEdit)
		{
			pLineEdit->setText(value.toString());
		}
	}

	virtual std::pair<bool, int> GetUsageInfo(const CAsset& asset, const string& pathToTest) const = 0;

};

class CUsedBy : public CDependenciesOperatorBase
{
public:
	virtual QString GetName() override { return QWidget::tr("used by"); }

	virtual bool    Match(const QVariant& value, const QVariant& filterValue) override
	{
		if (!filterValue.isValid())
		{
			return true;
		}

		const CAsset* const pAsset = value.isValid() ? reinterpret_cast<CAsset*>(value.value<intptr_t>()) : nullptr;
		if (!pAsset)
		{
			return false;
		}

		const string path(QtUtil::ToString(filterValue.toString()));
		return pAsset->IsAssetUsedBy(path).first;
	}

	virtual std::pair<bool, int> GetUsageInfo(const CAsset& asset, const string& pathToTest) const override
	{
		return asset.IsAssetUsedBy(pathToTest.c_str());
	}

};

class CUse : public CDependenciesOperatorBase
{
public:
	virtual QString GetName() override { return QWidget::tr("that use"); }

	virtual bool    Match(const QVariant& value, const QVariant& filterValue) override
	{
		if (!filterValue.isValid())
		{
			return true;
		}

		const CAsset* const pAsset = value.isValid() ? reinterpret_cast<CAsset*>(value.value<intptr_t>()) : nullptr;
		if (!pAsset)
		{
			return false;
		}

		const string path(QtUtil::ToString(filterValue.toString()));
		return pAsset->DoesAssetUse(path).first;
	}

	virtual std::pair<bool, int> GetUsageInfo(const CAsset& asset, const string& pathToTest) const override
	{
		return asset.DoesAssetUse(pathToTest.c_str());
	}

};

static CAttributeType<QString> s_dependenciesAttributeType({ new CUse(), new CUsedBy() });

class CDependenciesAttribute : public CItemModelAttribute
{
public:
	CDependenciesAttribute()
		: CItemModelAttribute("Dependencies", &s_dependenciesAttributeType, CItemModelAttribute::AlwaysHidden, true, QVariant(), (int)CAssetModel::Roles::InternalPointerRole)
	{
		static CAssetModel::CAutoRegisterColumn column(this, [](const CAsset* pAsset, const CItemModelAttribute* /*pAttribute*/, int role)
	    {
			return QVariant();
		});
	}
};

class CUsageCountAttribute : public CItemModelAttribute
{
public:
	CUsageCountAttribute()
		: CItemModelAttribute("Usage count", &Attributes::s_stringAttributeType, CItemModelAttribute::StartHidden, false)
	{
		static CAssetModel::CAutoRegisterColumn column(this, [](const CAsset* pAsset, const CItemModelAttribute* pAttribute, int role)
		{
			if (role != Qt::DisplayRole)
			{
				return QVariant();
			}
			const CUsageCountAttribute* const pUsageCountAttribute = static_cast<const CUsageCountAttribute*>(pAttribute);
			return pUsageCountAttribute->GetValue(*pAsset);
		});
	}

	void SetDetailContext(CAttributeFilter* pFilter)
	{
		m_pFilter = pFilter;
	}

	QVariant GetValue(const CAsset& asset) const
	{
		if (m_pFilter && m_pFilter->GetOperator())
		{
			const string filterValue = QtUtil::ToString(m_pFilter->GetFilterValue().toString());
			const auto usageInfo = static_cast<CDependenciesOperatorBase*>(m_pFilter->GetOperator())->GetUsageInfo(asset, filterValue);
			if (usageInfo.first && usageInfo.second != 0)
			{
				return usageInfo.second;
			}
		}

		return QVariant("n/a");
	}
private:
	CAttributeFilter* m_pFilter = nullptr;
};

static CDependenciesAttribute s_dependenciesAttribute;
static CUsageCountAttribute s_usageCountAttribute;

class SortFilterProxyModel : public QAttributeFilterProxyModel
{
	using QAttributeFilterProxyModel::QAttributeFilterProxyModel;

	class UsageCountAttributeContext
	{
	public:
		UsageCountAttributeContext(CAttributeFilter* pFilter)
		{
			s_usageCountAttribute.SetDetailContext(pFilter);
		}
		~UsageCountAttributeContext()
		{
			s_usageCountAttribute.SetDetailContext(nullptr);
		}
	};

	virtual void sort(int column, Qt::SortOrder order) override
	{
		UsageCountAttributeContext context(m_pDependencyFilter);
		QAttributeFilterProxyModel::sort(column, order);
	}

	//ensures folders and assets are always together in the sorting order
	bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
	{
		EAssetModelRowType leftType = (EAssetModelRowType)left.data((int)CAssetModel::Roles::TypeCheckRole).toUInt();
		EAssetModelRowType rightType = (EAssetModelRowType)right.data((int)CAssetModel::Roles::TypeCheckRole).toUInt();

		if (leftType == rightType)
		{
			// Comparing two variants will compare the types they contain, so it works as expected
			if (left.data(sortRole()) == right.data(sortRole()))
			{
				return left.data((int)CAssetModel::Roles::InternalPointerRole).value<intptr_t>() < right.data((int)CAssetModel::Roles::InternalPointerRole).value<intptr_t>();
			}
			else
			{
				return QAttributeFilterProxyModel::lessThan(left, right);
			}
		}
		else
		{
			return leftType == eAssetModelRow_Folder;
		}
	}

	bool rowMatchesFilter(int sourceRow, const QModelIndex& sourceParent) const
	{
		//specific handling for folders here so they are only tested for name
		QModelIndex index = sourceModel()->index(sourceRow, eAssetColumns_Name, sourceParent);
		if (!index.isValid())
			return false;

		EAssetModelRowType rowType = (EAssetModelRowType)index.data((int)CAssetModel::Roles::TypeCheckRole).toUInt();
		if (rowType == eAssetModelRow_Folder)
		{
			if (QDeepFilterProxyModel::rowMatchesFilter(sourceRow, sourceParent))
			{
				for (auto filter : m_filters)
				{
					if (filter->IsEnabled() && filter->GetAttribute() == &Attributes::s_nameAttribute)
					{
						QVariant val = sourceModel()->data(index, Qt::DisplayRole);
						if (!filter->Match(val))
						{
							return false;
						}
					}
				}
				return true;
			}
			return false;
		}
		else
		{
			return QAttributeFilterProxyModel::rowMatchesFilter(sourceRow, sourceParent);
		}
	}

	virtual bool canDropMimeData(const QMimeData* pMimeData, Qt::DropAction action, int row, int column, const QModelIndex& parent) const override
	{
		if (QAttributeFilterProxyModel::canDropMimeData(pMimeData, action, row, column, parent))
		{
			return true;
		}

		CDragDropData::ClearDragTooltip(qApp->widgetAt(QCursor::pos()));
		return false;
	}

	virtual QVariant data(const QModelIndex& index, int role) const override
	{
		UsageCountAttributeContext context(m_pDependencyFilter);
		return QAttributeFilterProxyModel::data(index, role);
	}

	virtual void InvalidateFilter() override
	{
		int usageCountFiltersCount = 0;
		m_pDependencyFilter = nullptr;
		for (const auto filter : m_filters)
		{
			if (!filter->IsEnabled())
			{
				continue;
			}

			if (filter->GetAttribute() == &s_dependenciesAttribute)
			{
				m_pDependencyFilter = ++usageCountFiltersCount == 1 ? filter.get() : nullptr;
			}
		}

		QAttributeFilterProxyModel::invalidateFilter();
	}

private:
	CAttributeFilter* m_pDependencyFilter = nullptr;
};

void GetExtensionFilter(ExtensionFilterVector& extFilter)
{
	CRY_ASSERT(CAssetManager::GetInstance()->GetAssetImporters().size() > 0);

	extFilter.resize(1);    // Reserve slot for "All supported types".

	std::vector<string> exts;
	for (CAssetImporter* pAssetImporter : CAssetManager::GetInstance()->GetAssetImporters())
	{
		for (const string& ext : pAssetImporter->GetFileExtensions())
		{
			stl::binary_insert_unique(exts, ext);
		}
	}

	QStringList allExts;
	for (const string& ext : exts)
	{
		extFilter << CExtensionFilter(QObject::tr(".%1 files").arg(ext.c_str()), ext.c_str());
		allExts << QtUtil::ToQString(ext);
	}

	extFilter << CExtensionFilter(QObject::tr("All Files (*.*)"), "*");

	const QString allExtsShort = QtUtil::ToQString(ShortenStringWithEllipsis(QtUtil::ToString(allExts.join(", "))));
	extFilter[0] = CExtensionFilter(QObject::tr("All importable files (%1)").arg(allExtsShort), allExts);
}

std::vector<CAsset*> GetAssets(const CDragDropData& data)
{
	QVector<quintptr> tmp;
	QByteArray byteArray = data.GetCustomData("Assets");
	QDataStream stream(byteArray);
	stream >> tmp;

	std::vector<CAsset*> assets;
	std::transform(tmp.begin(), tmp.end(), std::back_inserter(assets), [](quintptr p)
		{
			return reinterpret_cast<CAsset*>(p);
		});
	return assets;
}

bool IsMovePossible(const std::vector<CAsset*>& assets, const string& destinationFolder)
{
	// Do not allow moving to aliases, like %engine%
	if (destinationFolder.empty() || destinationFolder.front() == '%')
	{
		return false;
	}

	// Make sure none of assets belong to the destination folder.
	const string path(PathUtil::AddSlash(destinationFolder));
	return std::none_of(assets.begin(), assets.end(), [&path](CAsset* pAsset)
		{
			return strcmp(path.c_str(), pAsset->GetFolder()) == 0;
		});
}

// Implements QueryNewAsset for the AssetBrowser context menu.
class CContextMenuContext : public IUIContext
{
public:
	CContextMenuContext(CAssetBrowser* pBrowser)
		: m_pBrowser(pBrowser)
	{
		m_connection = QObject::connect(pBrowser, &CAssetBrowser::destroyed, [this](auto)
			{
				m_pBrowser = nullptr;
			});
	}
	~CContextMenuContext()
	{
		QObject::disconnect(m_connection);
	}

	virtual CAsset* QueryNewAsset(const CAssetType& type, const void* pTypeSpecificParameter) override
	{
		return m_pBrowser ? m_pBrowser->QueryNewAsset(type, pTypeSpecificParameter) : nullptr;
	}
private:
	CAssetBrowser*          m_pBrowser;
	QMetaObject::Connection m_connection;
};

void TryInstantEditing(CAsset* pAsset)
{
	if (!pAsset)
	{
		return;
	}

	CAssetEditor* pAssetEditor = pAsset->GetType()->GetInstantEditor();
	if (!pAssetEditor)
	{
		return;
	}

	CRY_ASSERT(pAssetEditor->CanOpenAsset(pAsset));

	CRY_ASSERT(GetIEditor()->FindDockableIf([pAssetEditor, pAsset](IPane* pPane, const string& className) -> bool
	{
		return pAssetEditor == pPane && pAsset->GetType()->GetInstantEditor() == static_cast<CAssetEditor*>(pPane);
	}));

	pAsset->Edit(pAssetEditor);

}

}

CAssetBrowser::CAssetBrowser(bool bHideEngineFolder /*= false*/, QWidget* pParent /*= nullptr*/)
	: CDockableEditor(pParent)
	, m_viewMode(Max)
	, m_recursiveView(false)
	, m_recursiveSearch(true)
	, m_navigationIndex(-1)
	, m_dontPushNavHistory(false)
{
	setObjectName("Asset Browser");

	InitViews(bHideEngineFolder);
	InitMenus();

	m_pAssetDropHandler.reset(new CAssetDropHandler());

	setAcceptDrops(true);

	//"Loading" feature while scanning for assets
	if (CAssetManager::GetInstance()->IsScanning())
	{
		//swap layout for a loading layout
		//swapping layout using the temporary widget trick
		auto tempWidget = new QWidget();
		tempWidget->setLayout(layout());

		QGridLayout* loadingLayout = new QGridLayout();
		loadingLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding), 0, 0, 3);
		loadingLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding), 1, 0);
		loadingLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding), 1, 2);
		loadingLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding), 2, 0);
		loadingLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding), 2, 2);
		loadingLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding), 3, 0, 3);
		loadingLayout->addWidget(new QLoading(), 1, 1, 1, 1, Qt::AlignHCenter | Qt::AlignBottom);
		loadingLayout->addWidget(new QLabel(tr("Loading Assets...")), 2, 1, 1, 1, Qt::AlignHCenter | Qt::AlignTop);
		setLayout(loadingLayout);

		CAssetManager::GetInstance()->signalScanningCompleted.Connect([loadingLayout, tempWidget, this]()
		{
			auto tempWidget2 = new QWidget();
			tempWidget2->setLayout(layout());
			setLayout(tempWidget->layout());
			tempWidget->deleteLater();
			tempWidget2->deleteLater();
			CAssetManager::GetInstance()->signalScanningCompleted.DisconnectById((uintptr_t)this);
		}, (uintptr_t)this);
	}
	InstallReleaseMouseFilter(this);
}

CAssetBrowser::~CAssetBrowser()
{
	CAssetManager::GetInstance()->signalScanningCompleted.DisconnectById((uintptr_t)this);
}

bool DiscardChanges(const QString& what)
{
	return CQuestionDialog::SQuestion("Discard changes?", what) == QDialogButtonBox::Yes;
}

void CAssetBrowser::mouseReleaseEvent(QMouseEvent* pEvent)
{
	switch (pEvent->button())
	{
	case Qt::MouseButton::BackButton:
		{
			if (m_backButton->isEnabled())
			{
				OnNavBack();
				pEvent->accept();
			}
			break;
		}
	case Qt::MouseButton::ForwardButton:
		{
			if (m_forwardButton->isEnabled())
			{
				OnNavForward();
				pEvent->accept();
			}
			break;
		}
	default:
		break;
	}
}

// Create and set item delegates for naming a new asset.
void CAssetBrowser::InitNewNameDelegates()
{
	auto onEnd = std::function<void(const QModelIndex&)>([this](const QModelIndex&)
	{
		EndCreateAsset();
	});

	m_detailsViewNewNameDelegate.reset(new CLineEditDelegate(m_detailsView));
	m_detailsViewNewNameDelegate->signalEditingAborted.Connect(onEnd);
	m_detailsViewNewNameDelegate->signalEditingFinished.Connect(onEnd);
	m_detailsView->setItemDelegate(m_detailsViewNewNameDelegate.get());

	QAbstractItemView* const pThumbnailView = m_thumbnailView->GetInternalView();
	m_thumbnailViewNewNameDelegate.reset(new CLineEditDelegate(pThumbnailView));
	m_thumbnailViewNewNameDelegate->signalEditingAborted.Connect(onEnd);
	m_thumbnailViewNewNameDelegate->signalEditingFinished.Connect(onEnd);
	pThumbnailView->setItemDelegate(m_thumbnailViewNewNameDelegate.get());
}

void CAssetBrowser::InitViews(bool bHideEngineFolder)
{
	using namespace Private_AssetBrowser;

	//Initialize models
	m_pFolderFilterModel.reset(new CAssetFolderFilterModel(false, true, this));

	m_pAttributeFilterProxyModel.reset(new Private_AssetBrowser::SortFilterProxyModel(QAttributeFilterProxyModel::BaseBehavior, this));
	m_pAttributeFilterProxyModel->setSourceModel(m_pFolderFilterModel.get());
	m_pAttributeFilterProxyModel->setFilterKeyColumn(eAssetColumns_FilterString);

	// TODO: Consider extracting the AssetsView stuff to a new CAssetsView class to encapsulate all the detail/thumbnail related states.
	InitAssetsView();
	QWidget* pAssetsView = CreateAssetsViewSelector();

	//filter panel
	m_filterPanel = new QFilteringPanel("AssetBrowser", m_pAttributeFilterProxyModel.get());
	//Searching in AB is likely to be quite expensive so timer prevents too many searches.
	//TODO : This should be an adaptive threshold depending on assets count!
	m_filterPanel->EnableDelayedSearch(true, 500.f);
	m_filterPanel->SetContent(pAssetsView);
	m_filterPanel->GetSearchBox()->setPlaceholderText(tr("Search Assets"));
	m_filterPanel->GetSearchBox()->signalOnFiltered.Connect(this, &CAssetBrowser::UpdateModels);
	m_filterPanel->signalOnFiltered.Connect(this, &CAssetBrowser::UpdateModels);

	//folders view
	m_foldersView = new CAssetFoldersView(bHideEngineFolder);
	m_foldersView->signalSelectionChanged.Connect(this, &CAssetBrowser::OnFolderSelectionChanged);
	connect(m_foldersView->m_treeView, &QTreeView::customContextMenuRequested, this, &CAssetBrowser::OnFolderViewContextMenu);

	m_foldersSplitter = new QSplitter();
	m_foldersSplitter->setOrientation(Qt::Horizontal);
	m_foldersSplitter->addWidget(m_foldersView);
	m_foldersSplitter->addWidget(m_filterPanel);
	m_foldersSplitter->setStretchFactor(0, 1);
	m_foldersSplitter->setStretchFactor(1, 3);

#if ASSET_BROWSER_USE_PREVIEW_WIDGET
	//preview widget, temporary solution
	m_previewWidget = new QContainer();
	m_foldersSplitter->addWidget(m_previewWidget);
	m_foldersSplitter->setStretchFactor(2, 3);
	m_previewWidget->setVisible(false);
#endif

	m_foldersSplitter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	//address bar

	//TODO : prev/next shortcuts unified with other places where we have it, use a generic command and unify with object create tool
	//TODO : hold on the buttons should "show" the history in a drop down, much like web browsers
	m_backButton = new QToolButton();
	m_backButton->setIcon(CryIcon("icons:General/Arrow_Left.ico"));
	m_backButton->setToolTip(tr("Back"));
	m_backButton->setEnabled(false);
	connect(m_backButton, &QToolButton::clicked, this, &CAssetBrowser::OnNavBack);

	m_forwardButton = new QToolButton();
	m_forwardButton->setIcon(CryIcon("icons:General/Arrow_Right.ico"));
	m_forwardButton->setToolTip(tr("Forward"));
	m_forwardButton->setEnabled(false);
	connect(m_forwardButton, &QToolButton::clicked, this, &CAssetBrowser::OnNavForward);

	m_breadcrumbs = new CBreadcrumbsBar();
	m_breadcrumbs->signalBreadcrumbClicked.Connect(this, &CAssetBrowser::OnBreadcrumbClick);
	m_breadcrumbs->signalTextChanged.Connect(this, &CAssetBrowser::OnBreadcrumbsTextChanged);
	m_breadcrumbs->SetValidator(std::function<bool(const QString)>([this](const QString path)
	{
		return this->ValidatePath(path);
	}));

	m_multipleFoldersLabel = new QLabel(tr("Multiple Folders Selected"));
	m_multipleFoldersLabel->hide();

	auto addressBar = new QHBoxLayout();
	addressBar->setMargin(0);
	addressBar->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	addressBar->addWidget(m_backButton);
	addressBar->addWidget(m_forwardButton);
	addressBar->addWidget(m_breadcrumbs);
	addressBar->addWidget(m_multipleFoldersLabel);

	UpdateBreadcrumbsBar(CAssetFoldersModel::GetInstance()->GetProjectAssetsFolderName());
	UpdateNavigation(false);

	//top level layout
	auto topLayout = new QVBoxLayout();
	topLayout->setMargin(0);
	topLayout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
	topLayout->addLayout(addressBar);
	topLayout->addWidget(m_foldersSplitter);

	//Layout
	SetContent(topLayout);

	SetViewMode(Thumbnails);//by default let's use thumbnails

	OnFolderSelectionChanged(m_foldersView->GetSelectedFolders());
}

void CAssetBrowser::InitMenus()
{
	// File menu
	AddToMenu(CEditor::MenuItems::FileMenu);
	CAbstractMenu* const menuFile = GetMenu(CEditor::MenuItems::FileMenu);
	menuFile->signalAboutToShow.Connect([menuFile, this]()
	{
		menuFile->Clear();
		auto folderSelection = m_foldersView->GetSelectedFolders();
		const QString folder = (folderSelection.size() == 1 && !CAssetFoldersModel::GetInstance()->IsReadOnlyFolder(folderSelection[0]))
		                       ? folderSelection[0]
		                       : QString();

		// Create assets.
		{
		  CAbstractMenu* subMenu = menuFile->CreateMenu(tr("New..."));
		  FillCreateAssetMenu(subMenu, folder);
		}

		const bool bEnableImport = !folder.isNull();

		auto action = menuFile->CreateAction(tr("Import"));
		action->setEnabled(bEnableImport);
		connect(action, &QAction::triggered, [this]() { OnImport(); });

		int section = menuFile->GetNextEmptySection();
		action = menuFile->CreateAction(tr("Save All"), section);
		connect(action, &QAction::triggered, [this]()
		{
			CProgressNotification notification(tr("Saving modified assets"), QString(), true);
			auto progress = [&notification](float value) { notification.SetProgress(value); };
			CAssetManager::GetInstance()->SaveAll(progress);
		});
	});

	// Edit menu
	AddToMenu(CEditor::MenuItems::EditMenu);
	CAbstractMenu* const menuEdit = GetMenu(CEditor::MenuItems::EditMenu);

	menuEdit->signalAboutToShow.Connect([menuEdit, this]()
	{
		menuEdit->Clear();

		auto action = menuEdit->CreateAction(tr("Generate All Thumbnails"));
		connect(action, &QAction::triggered, [this]()
		{
				GenerateThumbnailsAsync("");
		});

		action = menuEdit->CreateAction(tr("Generate/Repair All Metadata"));
		if (!CAssetManager::GetInstance()->IsScanning())
		{
		  connect(action, &QAction::triggered, []()
			{
				std::shared_ptr<CProgressNotification> pNotification = std::make_shared<CProgressNotification>(tr("Generating/Repairing Metadata"), QString(), false);
				CAssetManager::GetInstance()->GenerateCryassetsAsync([pNotification]()
				{
				});
			});
		}
		else
		{
		  action->setEnabled(false);
		}
	});

	//View menu
	AddToMenu(CEditor::MenuItems::ViewMenu);
	CAbstractMenu* const menuView = GetMenu(CEditor::MenuItems::ViewMenu);

	menuView->signalAboutToShow.Connect([menuView, this]()
	{
		menuView->Clear();

		auto action = menuView->CreateAction(tr("Details"));
		action->setCheckable(true);
		action->setChecked(m_viewMode == Details);
		connect(action, &QAction::triggered, this, [&]() { SetViewMode(Details); });

		action = menuView->CreateAction(tr("Thumbnails"));
		action->setCheckable(true);
		action->setChecked(m_viewMode == Thumbnails);
		connect(action, &QAction::triggered, this, [&]() { SetViewMode(Thumbnails); });

		action = menuView->CreateAction(tr("Split Horizontally"));
		action->setCheckable(true);
		action->setChecked(m_viewMode == HSplit);
		connect(action, &QAction::triggered, this, [&]() { SetViewMode(HSplit); });

		action = menuView->CreateAction(tr("Split Vertically"));
		action->setCheckable(true);
		action->setChecked(m_viewMode == VSplit);
		connect(action, &QAction::triggered, this, [&]() { SetViewMode(VSplit); });

		int sec = menuView->GetNextEmptySection();

		action = menuView->CreateAction(tr("Show Folder Tree"), sec);
		action->setCheckable(true);
		action->setChecked(m_foldersView->isVisible());
		connect(action, &QAction::triggered, this, [&]() { m_foldersView->setVisible(!m_foldersView->isVisible()); });

#if ASSET_BROWSER_USE_PREVIEW_WIDGET
		action = menuView->addAction(tr("Show Preview"), sec);
		action->setCheckable(true);
		action->setChecked(m_previewWidget->isVisible());
		connect(action, &QAction::triggered, this, [&]()
		{
			m_previewWidget->setVisible(!m_previewWidget->isVisible());
			UpdatePreview(m_selection->currentIndex());
		});
#endif

		sec = menuView->GetNextEmptySection();

		action = menuView->CreateAction(tr("Recursive View"), sec);
		action->setCheckable(true);
		action->setChecked(m_recursiveView);
		connect(action, &QAction::triggered, this, [&]() { SetRecursiveView(!m_recursiveView); });

		action = menuView->CreateAction(tr("Recursive Search"), sec);
		action->setCheckable(true);
		action->setEnabled(!m_recursiveView);
		action->setChecked(m_recursiveSearch || m_recursiveView);
		connect(action, &QAction::triggered, this, [&]() { SetRecursiveSearch(!m_recursiveSearch); });

		if (m_filterPanel)
		{
		  m_filterPanel->FillMenu(menuView, tr("Apply Filter"));
		}
	});
	
	s_signalMenuCreated(*GetMenu(MenuItems::ViewMenu), std::make_shared<Private_AssetBrowser::CContextMenuContext>(this));
}

void CAssetBrowser::InitAssetsView()
{
	//selection model must be shared with all the views
	m_selection = new QItemSelectionModel(m_pAttributeFilterProxyModel.get(), this);
	connect(m_selection, &QItemSelectionModel::currentChanged, this, &CAssetBrowser::OnCurrentChanged);
	connect(m_selection, &QItemSelectionModel::selectionChanged, [this](auto, auto)
	{
		SelectionChanged();
	});

	InitDetailsView();
	InitThumbnailsView();

	// Set up double-clicking.
	{
		typedef void (CAssetBrowser::* ResolveOverload)(const QModelIndex&);
		connect(m_detailsView, &QAdvancedTreeView::activated, this, (ResolveOverload) & CAssetBrowser::OnActivated);
		connect(m_thumbnailView->GetInternalView(), &QAbstractItemView::activated, this, (ResolveOverload) & CAssetBrowser::OnActivated);
	}

	InitNewNameDelegates();
}

void CAssetBrowser::InitDetailsView()
{
	using namespace Private_AssetBrowser;

	m_detailsView = new QAssetDetailsView();
	m_detailsView->setModel(m_pAttributeFilterProxyModel.get());
	m_detailsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_detailsView->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_detailsView->setSelectionModel(m_selection);
	m_detailsView->setUniformRowHeights(true);
	m_detailsView->setDragEnabled(true);
	m_detailsView->setDragDropMode(QAbstractItemView::DragDrop);
	m_detailsView->sortByColumn((int)eAssetColumns_Name, Qt::AscendingOrder);
	m_detailsView->setContextMenuPolicy(Qt::CustomContextMenu);
	m_detailsView->header()->setStretchLastSection(false);
	m_detailsView->header()->resizeSection((int)eAssetColumns_Name, fontMetrics().width(QStringLiteral("wwwwwwwwwwwwwwwwwwwwwwwwww")));
	m_detailsView->header()->resizeSection((int)eAssetColumns_Type, fontMetrics().width(QStringLiteral("wwwwwwwwwwwwwww")));
	m_detailsView->setTreePosition((int)eAssetColumns_Name);
	m_detailsView->setItemsExpandable(false);
	m_detailsView->setRootIsDecorated(false);
	m_detailsView->installEventFilter(this);
	m_detailsView->setEditTriggers(m_detailsView->editTriggers() & ~QAbstractItemView::DoubleClicked);

	connect(m_detailsView, &QTreeView::customContextMenuRequested, [this]() { OnContextMenu(); });

	FavoritesHelper::SetupView(m_detailsView, m_detailsView->GetAdvancedDelegate(), eAssetColumns_Favorite);
}

void CAssetBrowser::InitThumbnailsView()
{
	using namespace Private_AssetBrowser;

	m_thumbnailView = new QThumbnailsView(new CThumbnailsInternalView(), false, this);
	m_thumbnailView->SetModel(m_pAttributeFilterProxyModel.get());
	m_thumbnailView->SetRootIndex(QModelIndex());
	m_thumbnailView->signalShowContextMenu.Connect(this, &CAssetBrowser::OnContextMenu);
	m_thumbnailView->installEventFilter(this);
	QAbstractItemView* const pView = m_thumbnailView->GetInternalView();
	pView->setSelectionMode(QAbstractItemView::ExtendedSelection);
	pView->setSelectionBehavior(QAbstractItemView::SelectRows);
	pView->setSelectionModel(m_selection);
	pView->setDragDropMode(QAbstractItemView::DragDrop);
	pView->setEditTriggers(pView->editTriggers() & ~QAbstractItemView::DoubleClicked);
}

void CAssetBrowser::AddViewModeButton(ViewMode viewMode, const char* szIconPath, const char* szToolTip, QMenu* pMenu)
{
	QToolButton* const pButton = new QToolButton;
	connect(pButton, &QToolButton::clicked, this, [this, viewMode]() { SetViewMode(viewMode); });
	QString icon = QString(szIconPath);
	pButton->setIcon(CryIcon(icon));
	pButton->setCheckable(true);
	pButton->setAutoRaise(true);
	pButton->setToolTip(QObject::tr(szToolTip));
	pButton->setChecked(m_viewMode == viewMode);
	m_viewModeButtons->addButton(pButton, viewMode);

	if (pMenu)
	{
		pButton->setMenu(pMenu);
		pButton->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(pButton, &QToolButton::customContextMenuRequested, pButton, &QToolButton::showMenu);
	}
}

QWidget* CAssetBrowser::CreateAssetsViewSelector()
{
	QWidget* const pAssetsView = new QWidget();

	m_mainViewSplitter = new QSplitter();
	m_mainViewSplitter->setOrientation(Qt::Horizontal);
	m_mainViewSplitter->addWidget(m_detailsView);
	m_mainViewSplitter->addWidget(m_thumbnailView);

	// Setup View Mode Buttons

	m_viewModeButtons = new QButtonGroup(this);

	QMenu* const pThumbnailMenu = new QMenu(this);
	m_thumbnailSizeMenu.reset(new CAbstractMenu);
	m_thumbnailView->AppendPreviewSizeActions(*m_thumbnailSizeMenu.get());
	MenuWidgetBuilders::CMenuBuilder builder(pThumbnailMenu);
	m_thumbnailSizeMenu->Build(builder);

	AddViewModeButton(ViewMode::VSplit, "icons:common/general_view_vertical.ico", "Split Vertically\nShows both details and thumbnails");
	AddViewModeButton(ViewMode::HSplit, "icons:common/general_view_horizonal.ico", "Split Horizontally\nShows both details and thumbnails");
	AddViewModeButton(ViewMode::Details, "icons:common/general_view_list.ico", "Shows Details");
	AddViewModeButton(ViewMode::Thumbnails, "icons:common/general_view_thumbnail.ico", "Shows Thumbnails", pThumbnailMenu);

	QHBoxLayout* const pButtonsLayout = new QHBoxLayout();
	pButtonsLayout->setObjectName("viewModeButtonsLayout");
	pButtonsLayout->setContentsMargins(0, 0, 0, 0);
	pButtonsLayout->setMargin(0);
	pButtonsLayout->addSpacerItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
	const QList<QAbstractButton*> buttons = m_viewModeButtons->buttons();
	for (QAbstractButton* pButton : buttons)
	{
		pButtonsLayout->addWidget(pButton);
	}

	QVBoxLayout* const pLayout = new QVBoxLayout();
	pLayout->setSpacing(0);
	pLayout->setMargin(0);
	pLayout->addWidget(m_mainViewSplitter);
	pLayout->addLayout(pButtonsLayout);
	pAssetsView->setLayout(pLayout);

	return pAssetsView;
}

void CAssetBrowser::SelectAsset(const char* szPath) const
{
	CAsset* pAsset = CAssetManager::GetInstance()->FindAssetForFile(szPath);
	if (pAsset)
	{
		SelectAsset(*pAsset);
	}
	else if (strchr(szPath, '.')) // try to select folder by the file path
	{
		m_foldersView->SelectFolder(QtUtil::ToQString(PathUtil::GetDirectory(szPath)));
	}
	else
	{
		m_foldersView->SelectFolder(QtUtil::ToQString(szPath));
	}
}

void CAssetBrowser::SelectAsset(const CAsset& asset) const
{
	m_foldersView->SelectFolder(asset.GetFolder().c_str());
	QModelIndex idx = CAssetModel::GetInstance()->ToIndex(asset);
	QModelIndex result;
	QAbstractItemView* const pActiveView = m_viewMode == Thumbnails ? m_thumbnailView->GetInternalView() : m_detailsView;
	QtUtil::MapFromSourceIndirect(pActiveView, idx, result);
	m_selection->setCurrentIndex(result, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

// TODO: Only add menu entries for asset types that support creating new assets, i.e., implement CAssetType::Create().
void CAssetBrowser::FillCreateAssetMenu(CAbstractMenu* menu, const QString& folder)
{
	for (CAssetType* pAssetType : CAssetManager::GetInstance()->GetAssetTypes())
	{
		if (!pAssetType->CanBeCreated())
		{
			continue;
		}

		const bool bEnableAction = !folder.isNull();

		QAction* const pAction = menu->CreateAction(QtUtil::ToQString(pAssetType->GetUiTypeName()));
		connect(pAction, &QAction::triggered, [this, pAssetType]() { BeginCreateAsset(*pAssetType, nullptr); });
		pAction->setEnabled(bEnableAction);
	}
}

void CAssetBrowser::EditNewAsset()
{
	QAbstractItemView* pView = GetFocusedView();
	if (!pView)
	{
		if (m_viewMode == Thumbnails)
		{
			pView = m_thumbnailView->GetInternalView();
		}
		else
		{
			pView = m_detailsView;
		}
	}
	const int col = pView == m_thumbnailView->GetInternalView() ? eAssetColumns_Thumbnail : eAssetColumns_Name;

	const QModelIndex sourceIndex = CNewAssetModel::GetInstance()->index(0, col, QModelIndex());
	QModelIndex filteredIndex;
	if (!QtUtil::MapFromSourceIndirect(m_pAttributeFilterProxyModel.get(), sourceIndex, filteredIndex))
	{
		return;
	}

	if (filteredIndex.isValid())
	{
		pView->edit(filteredIndex);
		pView->scrollTo(filteredIndex);
		pView->selectionModel()->select(filteredIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	}
	else
	{
		CNewAssetModel::GetInstance()->setData(sourceIndex, "Untitled");
		EndCreateAsset();
	}
}

void CAssetBrowser::BeginCreateAsset(const CAssetType& type, const void* pTypeSpecificParameter)
{
	auto folderSelection = m_foldersView->GetSelectedFolders();
	string folder = QtUtil::ToString(folderSelection.front());
	if (folderSelection.size() != 1)
	{
		return; // More than one folder selected, so target folder is ambiguous.
	}

	CNewAssetModel::GetInstance()->BeginCreateAsset(folder, "Untitled", type, pTypeSpecificParameter);

	EditNewAsset();
}

void CAssetBrowser::EndCreateAsset()
{
	CNewAssetModel* const pModel = CNewAssetModel::GetInstance();
	pModel->EndCreateAsset();
	CAsset* pAsset = pModel->GetNewAsset();
	if (pAsset)
	{
		SelectAsset(*pAsset);
	}
}

CAsset* CAssetBrowser::QueryNewAsset(const CAssetType& type, const void* pTypeSpecificParameter)
{
	BeginCreateAsset(type, pTypeSpecificParameter);

	CNewAssetModel* const pModel = CNewAssetModel::GetInstance();
	while (CNewAssetModel::GetInstance()->IsEditing())
	{
		qApp->processEvents();
	}
	return pModel->GetNewAsset();
}

void CAssetBrowser::SetLayout(const QVariantMap& state)
{
	CDockableEditor::SetLayout(state);

	QVariant mainViewSplitter = state.value("mainViewSplitter");
	if (mainViewSplitter.isValid())
	{
		m_mainViewSplitter->restoreState(QByteArray::fromBase64(mainViewSplitter.toByteArray()));
	}

	QVariant foldersSplitter = state.value("foldersSplitter");
	if (foldersSplitter.isValid())
	{
		m_foldersSplitter->restoreState(QByteArray::fromBase64(foldersSplitter.toByteArray()));
	}

	QVariant viewModeVar = state.value("viewMode");
	if (viewModeVar.isValid())
	{
		SetViewMode((ViewMode)viewModeVar.toInt());
	}

	QVariant recursiveViewVar = state.value("recursiveView");
	if (recursiveViewVar.isValid())
	{
		SetRecursiveView(recursiveViewVar.toBool());
	}

	QVariant recursiveSearchVar = state.value("recursiveSearch");
	if (recursiveSearchVar.isValid())
	{
		SetRecursiveSearch(recursiveSearchVar.toBool());
	}

	QVariant showFoldersVar = state.value("showFolders");
	if (showFoldersVar.isValid())
	{
		m_foldersView->setVisible(showFoldersVar.toBool());
	}

#if ASSET_BROWSER_USE_PREVIEW_WIDGET
	QVariant showPreviewVar = state.value("showPreview");
	if (showPreviewVar.isValid())
	{
		m_previewWidget->setVisible(showPreviewVar.toBool());
	}
#endif

	QVariant filtersStateVar = state.value("filters");
	if (filtersStateVar.isValid() && filtersStateVar.type() == QVariant::Map)
	{
		m_filterPanel->SetState(filtersStateVar.value<QVariantMap>());
	}

	QVariant detailsViewVar = state.value("detailsView");
	if (detailsViewVar.isValid() && detailsViewVar.type() == QVariant::Map)
	{
		m_detailsView->SetState(detailsViewVar.value<QVariantMap>());
	}

	QVariant thumbnailViewVar = state.value("thumbnailView");
	if (thumbnailViewVar.isValid() && thumbnailViewVar.type() == QVariant::Map)
	{
		m_thumbnailView->SetState(thumbnailViewVar.value<QVariantMap>());
	}

	QVariant foldersViewVar = state.value("foldersView");
	if (foldersViewVar.isValid() && foldersViewVar.type() == QVariant::Map)
	{
		m_foldersView->SetState(foldersViewVar.value<QVariantMap>());
	}

	UpdateNavigation(true);
}

QVariantMap CAssetBrowser::GetLayout() const
{
	QVariantMap state = CDockableEditor::GetLayout();

	state.insert("mainViewSplitter", m_mainViewSplitter->saveState().toBase64());
	state.insert("foldersSplitter", m_foldersSplitter->saveState().toBase64());
	state.insert("viewMode", (int)m_viewMode);
	state.insert("recursiveView", m_recursiveView);
	state.insert("recursiveSearch", m_recursiveSearch);
	state.insert("showFolders", m_foldersView->isVisibleTo(this));
#if ASSET_BROWSER_USE_PREVIEW_WIDGET
	state.insert("showPreview", m_previewWidget->isVisibleTo(this));
#endif
	state.insert("filters", m_filterPanel->GetState());
	state.insert("detailsView", m_detailsView->GetState());
	state.insert("thumbnailView", m_thumbnailView->GetState());
	state.insert("foldersView", m_foldersView->GetState());

	return state;
}

QFilteringPanel* CAssetBrowser::GetFilterPanel()
{
	return m_filterPanel;
}

std::vector<CAsset*> CAssetBrowser::GetSelectedAssets() const
{
	std::vector<CAsset*> assets;
	std::vector<string> folders;
	ProcessSelection(assets, folders);
	return assets;
}

QStringList CAssetBrowser::GetSelectedFolders() const
{
	return m_foldersView->GetSelectedFolders();
}

CAsset* CAssetBrowser::GetLastSelectedAsset() const
{
	using namespace Private_AssetBrowser;

	auto index = m_selection->currentIndex();
	if (index.isValid() && IsAsset(index))
		return ToAsset(index);
	else
		return nullptr;
}

void CAssetBrowser::SetViewMode(ViewMode viewMode)
{
	if (m_viewMode != viewMode)
	{
		switch (viewMode)
		{
		case Details:
			m_thumbnailView->setVisible(false);
			m_detailsView->setVisible(true);
			break;
		case Thumbnails:
			m_thumbnailView->setVisible(true);
			m_detailsView->setVisible(false);
			break;
		case HSplit:
		case VSplit:
			m_thumbnailView->setVisible(true);
			m_detailsView->setVisible(true);
			m_mainViewSplitter->setOrientation(viewMode == VSplit ? Qt::Vertical : Qt::Horizontal);
			break;
		default:
			CRY_ASSERT(0);
			break;
		}

		m_viewMode = viewMode;

		CRY_ASSERT(m_viewModeButtons->buttons().size() == ViewMode::Max);
		for (int i = 0; i < ViewMode::Max; i++)
		{
			m_viewModeButtons->button(i)->setChecked(i == m_viewMode);
		}
	}
}

void CAssetBrowser::SetRecursiveView(bool recursiveView)
{
	if (m_recursiveView != recursiveView)
	{
		m_recursiveView = recursiveView;
		UpdateModels();
	}
}

void CAssetBrowser::SetRecursiveSearch(bool recursiveSearch)
{
	if (m_recursiveSearch != recursiveSearch)
	{
		m_recursiveSearch = recursiveSearch;
		UpdateModels();
	}
}

void CAssetBrowser::UpdateModels()
{
	//swaps to recursive view when recursive search is on
	bool searching = !m_filterPanel->GetSearchBox()->IsEmpty() || m_filterPanel->HasActiveFilters();

	if (searching && m_recursiveSearch && !m_pFolderFilterModel->IsRecursive())
	{
		m_pFolderFilterModel->SetShowFolders(false);
		m_pFolderFilterModel->SetRecursive(true);
	}
	else if (!searching && m_recursiveView != m_pFolderFilterModel->IsRecursive())
	{
		m_pFolderFilterModel->SetRecursive(m_recursiveView);
		m_pFolderFilterModel->SetShowFolders(!m_recursiveView);
	}
}

QAbstractItemView* CAssetBrowser::GetFocusedView() const
{
	QWidget* w = QApplication::focusWidget();
	if (w == m_thumbnailView->GetInternalView())
		return m_thumbnailView->GetInternalView();
	else if (w == m_detailsView)
		return m_detailsView;
	return nullptr;
}

bool CAssetBrowser::eventFilter(QObject* object, QEvent* event)
{
	using namespace Private_AssetBrowser;

	if (event->type() == QEvent::ToolTip)
	{
		if (object == m_detailsView)
		{
			auto index = m_detailsView->indexAt(m_detailsView->viewport()->mapFromGlobal(QCursor::pos()));
			auto asset = ToAsset(index);
			if (asset)
				CAssetTooltip::ShowTrackingTooltip(asset);
			else
				CAssetTooltip::HideTooltip();

			event->accept();
			return true;
		}

		if (object == m_thumbnailView)
		{
			auto index = m_thumbnailView->GetInternalView()->indexAt(m_thumbnailView->GetInternalView()->viewport()->mapFromGlobal(QCursor::pos()));
			auto asset = ToAsset(index);
			if (asset)
				CAssetTooltip::ShowTrackingTooltip(asset);
			else
				CAssetTooltip::HideTooltip();

			event->accept();
			return true;
		}
	}
	else if (event->type() == QEvent::MouseButtonRelease)
	{
		event->ignore();
		mouseReleaseEvent((QMouseEvent*)event);
		if (event->isAccepted())
			return true;
	}
	else if (event->type() == QEvent::Resize)
	{
		auto orientation = (width() > height()) ? Qt::Horizontal : Qt::Vertical;
		m_foldersSplitter->setOrientation(orientation);
	}

	return false;
}

void CAssetBrowser::ProcessSelection(std::vector<CAsset*>& assets, std::vector<string>& folders) const
{
	using namespace Private_AssetBrowser;

	auto indexList = m_selection->selectedRows(eAssetColumns_Name);
	assets.reserve(indexList.size());
	folders.reserve(indexList.size());
	for (auto& index : indexList)
	{
		const int type = GetType(index);
		switch (type)
		{
		case eAssetModelRow_Asset:
			{
				// The asset can be nullptr if we are in the process of creating a new asset. See CAssetBrowser::EditNewAsset()
				CAsset* const pAsset = ToAsset(index);
				if (pAsset)
				{
					assets.push_back(pAsset);
				}
			}
			break;
		case eAssetModelRow_Folder:
			folders.push_back(QtUtil::ToString(ToFolderPath(index)));
			break;
		default:
			CRY_ASSERT(0);
			break;
		}
	}
}

void CAssetBrowser::OnFolderViewContextMenu()
{
	CreateContextMenu(true);
}

void CAssetBrowser::OnContextMenu()
{
	CreateContextMenu(false);
}

void CAssetBrowser::CreateContextMenu(bool isFolderView /*= false*/)
{
	//TODO : This could be unified more with the folders view's context menu

	CAbstractMenu abstractMenu;

	std::vector<CAsset*> assets;
	std::vector<string> folders;
	ProcessSelection(assets, folders);

	if (!assets.empty())
	{
		BuildContextMenuForAssets(assets, folders, abstractMenu);
	}
	else if (!folders.empty() || isFolderView)
	{
		if (isFolderView)
		{
			folders = GetSelectedFoldersInFolderView();
		}
		BuildContextMenuForFolders(folders, abstractMenu);
	}
	else if (assets.empty() && folders.empty() && !m_recursiveView)//nothing selected in recursive view
	{
		BuildContextMenuForEmptiness(abstractMenu);
	}

	QMenu menu;
	abstractMenu.Build(MenuWidgetBuilders::CMenuBuilder(&menu));

	if (menu.actions().count() > 0)
	{
		menu.exec(QCursor::pos());
	}
}

void CAssetBrowser::BuildContextMenuForEmptiness(CAbstractMenu &abstractMenu)
{
	std::vector<string> selectedFolders = GetSelectedFoldersInFolderView();
	CAssetFoldersModel* pModel = CAssetFoldersModel::GetInstance();

	int foldersSection = abstractMenu.GetNextEmptySection();
	abstractMenu.SetSectionName(foldersSection, "Folders");

	auto folder = QtUtil::ToQString(selectedFolders[0]);
	if (selectedFolders.size() == 1 && !pModel->IsReadOnlyFolder(folder))
	{
		auto action = abstractMenu.CreateAction(CryIcon("icons:General/Element_Add.ico"), tr("Create folder"));
		connect(action, &QAction::triggered, [this, folder]() { OnCreateFolder(folder); });

		CAbstractMenu* const pCreateAssetMenu = abstractMenu.CreateMenu(tr("New..."));
		FillCreateAssetMenu(pCreateAssetMenu, folder);

		action = abstractMenu.CreateAction(tr("Import"), foldersSection);
		connect(action, &QAction::triggered, [this]() { OnImport(); });

		action = abstractMenu.CreateAction(tr("Show in File Explorer"), foldersSection);
		connect(action, &QAction::triggered, [this, folder]() { OnOpenInExplorer(folder); });

		action = abstractMenu.CreateAction(tr("Generate Thumbnails"), foldersSection);
		connect(action, &QAction::triggered, [this, folder]()
		{
			GenerateThumbnailsAsync(QtUtil::ToString(folder));
		});
	}

	NotifyContextMenuCreation(abstractMenu, {}, selectedFolders);
}

std::vector<string> CAssetBrowser::GetSelectedFoldersInFolderView()
{
	auto& selectedFolders = m_foldersView->GetSelectedFolders();

	std::vector<string> folders;
	folders.reserve(selectedFolders.size());
	std::transform(selectedFolders.cbegin(), selectedFolders.cend(), std::back_inserter(folders), [](const QString& str)
	{	
		return QtUtil::ToString(str);
	});
	return folders;
}
void CAssetBrowser::BuildContextMenuForFolders(const std::vector<string>& folders, CAbstractMenu &abstractMenu)
{
	if (folders.size() > 1)
	{
		return;
	}

	//Do not show folder actions if we are not showing folder
	auto folder = QtUtil::ToQString(folders[0]);
	if (CAssetFoldersModel::GetInstance()->IsReadOnlyFolder(folder))
		return;

	//TODO : move this, just only add the separator if we add more things later
	int foldersSection = abstractMenu.GetNextEmptySection();
	abstractMenu.SetSectionName(foldersSection, "Folders");

	auto action = abstractMenu.CreateAction(CryIcon("icons:General/Element_Add.ico"), tr("Create folder"));
	connect(action, &QAction::triggered, [this, folder]() { OnCreateFolder(folder); });

	if (CAssetFoldersModel::GetInstance()->IsEmptyFolder(folder))
	{
		action = abstractMenu.CreateAction(CryIcon("icons:General/Element_Remove.ico"), tr("Delete"));
		connect(action, &QAction::triggered, [this, folder]()
		{
			CAssetFoldersModel::GetInstance()->DeleteFolder(folder);
		});

		action = abstractMenu.CreateAction(tr("Rename"), foldersSection);
		connect(action, &QAction::triggered, [this, folder]() { OnRenameFolder(folder); });
	}

	action = abstractMenu.CreateAction(tr("Show in File Explorer"), foldersSection);
	connect(action, &QAction::triggered, [this, folder]()
	{
		CAssetFoldersModel::GetInstance()->OpenFolderWithShell(folder);
	});

	action = abstractMenu.CreateAction(tr("Generate Thumbnails"), foldersSection);
	connect(action, &QAction::triggered, [this, folder = folders[0]]()
	{
		GenerateThumbnailsAsync(folder);
	});

	NotifyContextMenuCreation(abstractMenu, {}, folders);
}

void CAssetBrowser::BuildContextMenuForAssets(const std::vector<CAsset*>& assets, const std::vector<string>& folders, CAbstractMenu &abstractMenu)
{
	bool canReimport = false;
	bool isReadOnly = false;
	bool isModified = false;
	QMap<const CAssetType*, std::vector<CAsset*>> assetsByType;

	for (CAsset* asset : assets)
	{
	if (asset->GetType()->IsImported() && !asset->IsReadOnly() && asset->HasSourceFile())
		{
			canReimport = true;
		}

		if (asset->IsReadOnly() || !GetISystem()->GetIPak()->IsFileExist(assets.front()->GetFile(0), ICryPak::eFileLocation_OnDisk))
		{
			isReadOnly = true;
		}

		isModified = isModified || asset->IsModified();

		assetsByType[asset->GetType()].push_back(asset);
	}

	int section = abstractMenu.FindSectionByName("Assets");

	if (canReimport)
	{
		auto action = abstractMenu.CreateAction(tr("Reimport"), section);
		connect(action, &QAction::triggered, [this, assets]() { OnReimport(assets); });
	}

	if (!isReadOnly)
	{
		auto action = abstractMenu.CreateAction(tr("Delete"));
		connect(action, &QAction::triggered, [this, assets]() { OnDelete(assets); });
	}

	if (isModified)
	{
		auto action = abstractMenu.CreateAction(tr("Save"));
		connect(action, &QAction::triggered, [this, assets]() 
		{  
			for (CAsset* pAsset : assets)
			{
				pAsset->Save();
			}
		});

		action = abstractMenu.CreateAction(tr("Discard Changes"));
		connect(action, &QAction::triggered, [this, assets]()
		{
			const QString title(tr("Discard Changes"));
			const QString text(tr("Are you sure you want to discard the changes in the selected assets?"));

			const auto button = CQuestionDialog::SQuestion(title, text, QDialogButtonBox::Discard | QDialogButtonBox::Cancel, QDialogButtonBox::Cancel);
			if (QDialogButtonBox::Discard == button)
			{
				for (CAsset* pAsset : assets)
				{
					pAsset->Reload();
				}
			}
		});
	}

	//TODO : source control
	auto it = assetsByType.begin();
	for (; it != assetsByType.end(); ++it)
	{
		if (it->size() != 0)
		{
			const CAssetType* pType = it.key();
			string s = pType->GetTypeName();
			std::vector<CAsset*> assets = it.value();
			pType->AppendContextMenuActions(assets, &abstractMenu);
		}
	}

	if (assets.size() == 1)
	{
		const bool isAssetOnDisk = GetISystem()->GetIPak()->IsFileExist(assets.front()->GetFile(0), ICryPak::eFileLocation_OnDisk);

		auto action = abstractMenu.CreateAction(tr("Rename"));
		action->setDisabled(!isAssetOnDisk || assets.front()->IsReadOnly());
		connect(action, &QAction::triggered, [this, pAsset = assets.front()]() { OnRenameAsset(*pAsset); });

		action = abstractMenu.CreateAction(tr("Show in File Explorer"));
		action->setDisabled(!isAssetOnDisk);
		connect(action, &QAction::triggered, [this, pAsset = assets.front()]()
		{
			const string path = PathUtil::Make(PathUtil::GetGameProjectAssetsPath(), pAsset->GetFile(0));
			QtUtil::OpenInExplorer(path);
		});

		AppendFilterDependenciesActions(&abstractMenu, assets.front());
	}

	NotifyContextMenuCreation(abstractMenu, assets, folders);
}

void CAssetBrowser::NotifyContextMenuCreation(CAbstractMenu& menu, const std::vector<CAsset*>& assets, const std::vector<string>& folders) 
{
	if (menu.FindSectionByName("Assets") == CAbstractMenu::eSections_Default)
	{
		int section = menu.GetNextEmptySection();
		menu.SetSectionName(section, "Assets");
	}

	s_signalContextMenuRequested(menu, assets, folders, std::make_shared<Private_AssetBrowser::CContextMenuContext>(this));
}

void CAssetBrowser::AppendFilterDependenciesActions(CAbstractMenu* pAbstractMenu, const CAsset* pAsset)
{
	using namespace Private_AssetBrowser;

	const auto dependencyOperators = s_dependenciesAttribute.GetType()->GetOperators();
	for (Attributes::IAttributeFilterOperator* pOperator : dependencyOperators)
	{
		QAction* pAction = pAbstractMenu->CreateAction(QString("%1 %2 '%3'").arg(tr("Show Assets"), pOperator->GetName(), QtUtil::ToQString(pAsset->GetName())));
		connect(pAction, &QAction::triggered, [pOperator, pAsset]()
		{
			CAssetBrowser* const pAssetBrowser = static_cast<CAssetBrowser*>(GetIEditor()->CreateDockable("Asset Browser"));
			if (pAssetBrowser)
			{
				pAssetBrowser->GetFilterPanel()->AddFilter(s_dependenciesAttribute.GetName(), pOperator->GetName(), QtUtil::ToQString(pAsset->GetFile(0)));
				pAssetBrowser->GetFilterPanel()->SetExpanded(true);
				pAssetBrowser->SetRecursiveView(true);
			}
		});
	}
}

void CAssetBrowser::OnActivated(const QModelIndex& index)
{
	using namespace Private_AssetBrowser;

	const int type = GetType(index);
	switch (type)
	{
	case eAssetModelRow_Asset:
		{
			CAsset* pAsset = ToAsset(index);
			if (pAsset)
			{
				OnActivated(pAsset);
			}
			break;
		}
	case eAssetModelRow_Folder:
		{
		OnActivated(ToFolderPath(index));
			break;
		}
	default:
		CRY_ASSERT(0);
		break;
	}
}

void CAssetBrowser::OnActivated(CAsset* pAsset)
{
	if (m_pQuickEditTimer)
	{
		m_pQuickEditTimer->stop();
	}
	pAsset->Edit();
}

void CAssetBrowser::OnActivated(const QString& folder)
{
	m_foldersView->SelectFolder(folder);
}

void CAssetBrowser::OnCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
	if (current.isValid())
	{
		//selections are in sync but views and scrolling is not always
		m_thumbnailView->ScrollToRow(current);
		m_detailsView->scrollTo(current);
		UpdatePreview(current);
	}
}

void CAssetBrowser::UpdatePreview(const QModelIndex& currentIndex)
{
	using namespace Private_AssetBrowser;

	if (IsAsset(currentIndex))
	{
		CAsset* const pAsset = ToAsset(currentIndex);
		if (pAsset && pAsset->GetType()->GetInstantEditor())
		{
			if (!m_pQuickEditTimer)
			{
				m_pQuickEditTimer.reset(new QTimer());
				m_pQuickEditTimer->setSingleShot(true);
				m_pQuickEditTimer->setInterval(200);

				connect(m_pQuickEditTimer.get(), &QTimer::timeout, [this]()
				{
					QModelIndex currentIndex = m_selection->currentIndex();
					CAsset* pAsset = currentIndex.isValid() ? ToAsset(currentIndex) : nullptr;
					TryInstantEditing(pAsset);
				});
			}

			m_pQuickEditTimer->start();
		}
	}

#if ASSET_BROWSER_USE_PREVIEW_WIDGET
	if (m_previewWidget->isVisible())
	{
		if (IsAsset(currentIndex))
		{
			CAsset* asset = ToAsset(currentIndex);
			if (asset)
			{
				QWidget* w = asset->GetType()->CreatePreviewWidget(asset);
				if (w)
				{
					m_previewWidget->SetChild(w);
					return;
				}
			}
		}

		m_previewWidget->SetChild(nullptr);
	}
#endif
}

void CAssetBrowser::OnImport()
{
	using namespace Private_AssetBrowser;

	// If there are no imports, there are no supported extensions, so we cannot show the file dialog.
	if (!CAssetManager::GetInstance()->GetAssetImporters().size())
	{
		const QString what = tr(
			"No importers available. This might be because you are missing editor plugins. "
			"If you build Sandbox locally, check if all plugins have been built successfully. "
			"If not, make sure that all required dependencies and SDKs are available.");
		CQuestionDialog::SWarning(tr("No importers registered"), what);
		return;
	}

	static const char* const szRecentImportPathProperty = "RecentImportPath";

	std::vector<string> filePaths;
	{
		CSystemFileDialog::RunParams runParams;
		GetExtensionFilter(runParams.extensionFilters);

		const QString recentImportPath = GetProjectProperty(szRecentImportPathProperty).toString();
		if (!recentImportPath.isEmpty())
		{
			runParams.initialDir = recentImportPath;
		}

		std::vector<QString> v = CSystemFileDialog::RunImportMultipleFiles(runParams, nullptr);
		filePaths.reserve(v.size());
		std::transform(v.begin(), v.end(), std::back_inserter(filePaths), QtUtil::ToString);
	}

	if (filePaths.empty())
	{
		return;
	}

	SetProjectProperty(szRecentImportPathProperty, PathUtil::GetPathWithoutFilename(filePaths[0]).c_str());

	CAssetDropHandler dropHandler;
	if (dropHandler.CanImportAny(filePaths))
	{
		CAssetDropHandler::SImportParams importParams;
		auto folderSelection = m_foldersView->GetSelectedFolders();
		if (folderSelection.size() == 1)
		{
			importParams.outputDirectory = QtUtil::ToString(folderSelection.front());
		}
		ThreadingUtils::AsyncFinalize([dropHandler, filePaths, importParams]
		{
			return dropHandler.Import(filePaths, importParams);
		},
		                              [](std::vector<CAsset*>&& assets)
		{
			GetIEditor()->GetAssetManager()->MergeAssets(assets);
		});
	}
	else
	{
		if (filePaths.size() > 1)
		{
			CryWarning(VALIDATOR_MODULE_ASSETS, VALIDATOR_ERROR, "Cannot import files.");
		}
		else
		{
			CryWarning(VALIDATOR_MODULE_ASSETS, VALIDATOR_ERROR, "Cannot import file '%s'.", filePaths.front().c_str());
		}
	}
}

void CAssetBrowser::OnReimport(const std::vector<CAsset*>& assets)
{
	for (CAsset* pAsset : assets)
	{
		if (pAsset->GetType()->IsImported() && !pAsset->IsReadOnly())
		{
			pAsset->Reimport();
		}
	}
}

void CAssetBrowser::OnDelete(const std::vector<CAsset*>& assets)
{
	CRY_ASSERT(std::none_of(assets.begin(), assets.end(), [](CAsset* pAsset) { return !pAsset; }));

	std::vector<CAsset*> assetsToDelete(assets);
	CAssetManager* const pAssetManager = CAssetManager::GetInstance();

	const QString question = tr("There is a possibility of undetected dependencies which can be violated after performing the operation.\n"
	                            "\n"
	                            "Do you really want to delete %n asset(s)?", "", assets.size());

	if (pAssetManager->HasAnyReverseDependencies(assetsToDelete))
	{
		CAssetReverseDependenciesDialog assetDeleteDialog(
			assets,
			tr("Assets to be deleted"),
			tr("Dependent Assets"),
			tr("The following assets depend on the asset(s) to be deleted. Therefore they probably will not behave correctly after performing the delete operation."),
			question,
			this);
		assetDeleteDialog.setWindowTitle(tr("Delete Assets"));

		if (!assetDeleteDialog.Execute())
		{
			return;
		}
	}
	else if (CQuestionDialog::SQuestion(tr("Delete Assets"), question) != QDialogButtonBox::Yes)
	{
		return;
	}

	pAssetManager->DeleteAssetsWithFiles(assetsToDelete);
}

bool CAssetBrowser::OnOpen()
{
	const std::vector<CAsset*> assets = GetSelectedAssets();
	if (assets.empty())
	{
		return false;
	}

	for (CAsset* pAsset : assets)
	{
		OnActivated(pAsset);
	}
	return true;
}

void CAssetBrowser::OnRenameFolder(const QString& folder)
{
	auto view = GetFocusedView();
	if (!view)
	{
		return;
	}

	QModelIndex sourceIndex = CAssetFoldersModel::GetInstance()->FindIndexForFolder(folder);

	auto column = view == m_detailsView ? EAssetColumns::eAssetColumns_Name : EAssetColumns::eAssetColumns_Thumbnail;
	sourceIndex = sourceIndex.sibling(sourceIndex.row(), column);
	QModelIndex index;
	if (QtUtil::MapFromSourceIndirect(view, sourceIndex, index))
	{
		m_selection->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
		view->edit(index);
	}
}

void CAssetBrowser::OnCreateFolder(const QString& parentFolder)
{
	QString newFolderPath = CAssetFoldersModel::GetInstance()->CreateFolder(parentFolder);
	OnRenameFolder(newFolderPath);
}

void CAssetBrowser::OnOpenInExplorer(const QString& folder)
{
	CAssetFoldersModel::GetInstance()->OpenFolderWithShell(folder);
}

void CAssetBrowser::OnRenameAsset(CAsset& asset)
{
	auto view = GetFocusedView();
	if (!view)
	{
		return;
	}

	auto column = view == m_detailsView ? EAssetColumns::eAssetColumns_Name : EAssetColumns::eAssetColumns_Thumbnail;
	QModelIndex sourceIndex = CAssetModel::GetInstance()->ToIndex(asset, column);
	QModelIndex index;
	if (QtUtil::MapFromSourceIndirect(view, sourceIndex, index))
	{
		m_selection->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
		view->edit(index);
	}
}

void CAssetBrowser::GenerateThumbnailsAsync(const string& folder, const std::function<void()>& finalize /*= std::function<void()>()*/)
{
	AsseThumbnailsGenerator::GenerateThumbnailsAsync(folder, finalize);
}

void CAssetBrowser::OnNavBack()
{
	m_dontPushNavHistory = true;

	if (m_navigationIndex >= 0)
	{
		m_navigationIndex--;
	}

	if (m_navigationIndex == -1)
		m_foldersView->ClearSelection();
	else
		m_foldersView->SelectFolders(m_navigationHistory[m_navigationIndex]);

	m_dontPushNavHistory = false;
}

void CAssetBrowser::OnNavForward()
{
	m_dontPushNavHistory = true;

	m_navigationIndex++;
	m_foldersView->SelectFolders(m_navigationHistory[m_navigationIndex]);

	m_dontPushNavHistory = false;
}

void CAssetBrowser::OnFolderSelectionChanged(const QStringList& selectedFolders)
{
	using namespace Private_AssetBrowser;

	CThumbnailsInternalView* pThumbnailsView = static_cast<CThumbnailsInternalView*>(m_thumbnailView->GetInternalView());
	QAssetDetailsView* pDetailsView = static_cast<QAssetDetailsView*>(m_detailsView);

	const int numFolders = selectedFolders.size();
	if (numFolders > 1)
	{
		m_breadcrumbs->hide();
		m_multipleFoldersLabel->show();

		pThumbnailsView->SetRootFolder(QString());
		pDetailsView->SetRootFolder(QString());
	}
	else
	{
		m_breadcrumbs->show();
		m_multipleFoldersLabel->hide();

		UpdateBreadcrumbsBar(CAssetFoldersModel::GetInstance()->GetPrettyPath(selectedFolders.first()));

		pThumbnailsView->SetRootFolder(selectedFolders.first());
		pDetailsView->SetRootFolder(selectedFolders.first());
	}

	m_pFolderFilterModel->SetAcceptedFolders(selectedFolders);

	if (!m_dontPushNavHistory)
	{
		if (m_navigationIndex < m_navigationHistory.count() - 1)
			m_navigationHistory.resize(m_navigationIndex + 1);

		m_navigationHistory.append(selectedFolders);
		m_navigationIndex++;
	}

	UpdateNavigation(false);

	SelectionChanged();
}

void CAssetBrowser::UpdateNavigation(bool clearHistory)
{
	if (clearHistory)
	{
		m_navigationHistory.clear();
		m_navigationIndex = -1;
	}

	m_backButton->setEnabled(m_navigationHistory.count() > 0 && m_navigationIndex > -1);
	m_forwardButton->setEnabled(m_navigationHistory.count() && m_navigationIndex < m_navigationHistory.count() - 1);
}

void CAssetBrowser::UpdateBreadcrumbsBar(const QString& path)
{
	m_breadcrumbs->Clear();

	int curIndex = 0;
	int slashIndex = -1;

	do
	{
		slashIndex = path.indexOf('/', curIndex);
		QString crumbText = path.mid(curIndex, slashIndex == -1 ? -1 : slashIndex - curIndex);
		m_breadcrumbs->AddBreadcrumb(crumbText, path.mid(0, slashIndex));
		curIndex = slashIndex + 1;
	}
	while (slashIndex != -1);
}

void CAssetBrowser::OnBreadcrumbClick(const QString& text, const QVariant& data)
{
	auto index = CAssetFoldersModel::GetInstance()->FindIndexForFolder(data.toString(), CAssetFoldersModel::Roles::DisplayFolderPathRole);
	if (index.isValid())
	{
		m_foldersView->SelectFolder(index);
	}
}

void CAssetBrowser::OnBreadcrumbsTextChanged(const QString& text)
{
	auto index = CAssetFoldersModel::GetInstance()->FindIndexForFolder(text, CAssetFoldersModel::Roles::DisplayFolderPathRole);
	if (index.isValid())
	{
		m_foldersView->SelectFolder(index);
	}
	else
	{
		// Check if the user entered the absolute path and delete up to the asset folder
		// fromNativeSeparators ensures same seperators are used
		QString assetsPaths = QDir::fromNativeSeparators(QDir::currentPath());
		QString breadCrumbsPath = QDir::fromNativeSeparators(text);
		if (breadCrumbsPath.contains(assetsPaths))
		{
			breadCrumbsPath.remove(assetsPaths);
		}
		auto index = CAssetFoldersModel::GetInstance()->FindIndexForFolder(breadCrumbsPath, CAssetFoldersModel::Roles::DisplayFolderPathRole);
		if (index.isValid())
		{
			m_foldersView->SelectFolder(index);
		}
	}
}

bool CAssetBrowser::OnFind()
{
	m_filterPanel->GetSearchBox()->setFocus();
	return true;
}

bool CAssetBrowser::ValidatePath(const QString path)
{
	auto index = CAssetFoldersModel::GetInstance()->FindIndexForFolder(path, CAssetFoldersModel::Roles::DisplayFolderPathRole);
	if (index.isValid())
	{
		return true;
	}
	else
	{
		// Check if the user entered the absolute path and delete up to the asset folder
		// fromNativeSeparators ensures same seperators are used
		QString assetsPaths = QDir::fromNativeSeparators(QDir::currentPath());
		QString breadCrumbsPath = QDir::fromNativeSeparators(path);
		if (breadCrumbsPath.contains(assetsPaths))
		{
			breadCrumbsPath.remove(assetsPaths);
		}
		auto index = CAssetFoldersModel::GetInstance()->FindIndexForFolder(breadCrumbsPath, CAssetFoldersModel::Roles::DisplayFolderPathRole);
		if (index.isValid())
		{
			return true;
		}
	}
	return false;
}

bool CAssetBrowser::OnDelete()
{
	const std::vector<CAsset*> assets = GetSelectedAssets();
	if (!assets.empty())
	{
		OnDelete(assets);
	}
	return true;
}

QAttributeFilterProxyModel* CAssetBrowser::GetAttributeFilterProxyModel()
{
	return m_pAttributeFilterProxyModel.get();
}

QItemSelectionModel* CAssetBrowser::GetItemSelectionModel()
{
	return m_selection;
}

QAdvancedTreeView* CAssetBrowser::GetDetailsView()
{
	return m_detailsView;
}

QThumbnailsView* CAssetBrowser::GetThumbnailsView()
{
	return m_thumbnailView;
}

void CAssetBrowser::ScrollToSelected()
{
	const QModelIndex index = GetItemSelectionModel()->currentIndex();
	if (index.isValid())
	{
		m_detailsView->scrollTo(index, QAbstractItemView::EnsureVisible);
		m_thumbnailView->ScrollToRow(index, QAbstractItemView::EnsureVisible);
	}
}
