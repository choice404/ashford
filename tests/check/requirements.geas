// A requirements block over named subcontracts and a loose pledge, with
// grouping and negation, plus an anonymous subcontract whose pledge joins the
// contract pledge namespace and stays out of the policy.

contract Shipping {
    subcontract Packing {
        pledge pack(order: Order) -> Result<Bool, PackError>
    }

    subcontract Delivery {
        pledge dispatch(order: Order) -> Result<Bool, ShipError>
        pledge confirm(order: Order) -> Result<Bool, ShipError>
    }

    subcontract {
        pledge audit_trail(order: Order) -> Result<Unit, AuditError>
    }

    pledge invoice(order: Order) -> Result<Receipt, BillError>

    requirements {
        fulfill: (Packing && Delivery) && invoice
        partial: Packing || Delivery || invoice
        break: !Packing && !Delivery && !invoice
    }
}
